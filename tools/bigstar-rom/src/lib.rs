use anyhow::{anyhow, bail, Context, Result};
use ds_rom::{compress::lz77::Lz77, crc::CRC_16_MODBUS};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    path::{Path, PathBuf},
};

const BX_LR: u32 = 0xE12F_FF1E;
const POP_PC: u32 = 0xE8BD_8000;
const NOP: u32 = 0xE1A0_0000;
const ARM9_COMPRESSION_START: usize = 0x4000;
const ARM9_FOOTER_SIZE: usize = 12;
const HEADER_SIZE: usize = 0x4000;
const MVL_RUNTIME_CONFIG_ADDR: u32 = 0x020C_5360;
const MVL_RUNTIME_CONFIG_MAGIC: u32 = 0x434C_564D; // "MVLC", little endian
const MVL_RUNTIME_CONFIG_STAGE_OFFSET: u32 = 0x04;
const MVL_RUNTIME_CONFIG_SCENE_SETTINGS_OFFSET: u32 = 0x08;
const MVL_RUNTIME_CONFIG_INITIAL_LIVES_OFFSET: u32 = 0x0C;
const MVL_RUNTIME_CONFIG_LIFE_MODE_SELECTOR_OFFSET: u32 = 0x10;
const MVL_RUNTIME_CONFIG_BIG_STAR_SELECTOR_OFFSET: u32 = 0x14;
const MVL_NATIVE_COURSE_SELECTOR_ADDR: u32 = 0x0215_C890;
const GAME_PLAYER_INVENTORY_POWERUP_ADDR: u32 = 0x0208_B32C;
const PLAYER_BASE_REQUESTED_POWERUP_OFFSET: u32 = 0x7AB;
const PLAYER_BASE_CURRENT_POWERUP_OFFSET: u32 = 0x7AC;
const PLAYER_BASE_PREVIOUS_POWERUP_OFFSET: u32 = 0x7AD;
const PLAYER_POWERUP_MEGA: u32 = 3;
const GAME_TICK_PROBE_REQUEST_ADDR: u32 = 0x0200_1AC0;
const GAME_TICK_PROBE_ACTIVE_ADDR: u32 = 0x0200_1AC4;
const GAME_TICK_PROBE_MAGIC_ADDR: u32 = 0x0200_1AC8;
const GAME_TICK_PROBE_HISTORY_ENABLED_ADDR: u32 = 0x0200_1ACC;
const GAME_TICK_PROBE_HISTORY_INDEX_ADDR: u32 = 0x0200_1AD0;
const GAME_TICK_PROBE_HISTORY_COUNT_ADDR: u32 = 0x0200_1AD4;
const GAME_TICK_PROBE_HISTORY_TARGET_ADDR: u32 = 0x0200_1AD8;
const GAME_TICK_PROBE_HISTORY_START_FRAME_ADDR: u32 = 0x0200_1ADC;
// The JIT scratch packets occupy 0x023C1240..0x023C12B3. Keep the replay
// history in the following reserved scratch page so all twelve entries can
// retain per-player touch metadata without shrinking the rollback window.
const GAME_TICK_PROBE_HISTORY_ADDR: u32 = 0x023C_1300;
const GAME_TICK_PROBE_HISTORY_CAPACITY: usize = 12;
const GAME_TICK_PROBE_HISTORY_ENTRY_BYTES: usize = 16;
const GAME_TICK_PROBE_MAGIC: u32 = 0x3250_5447; // "GTP2", little endian
const GAME_TICK_PROBE_PACKET_TICK_ADDR: u32 = 0x0208_88E0;
const GAME_TICK_PROBE_JIT_SCRATCH_TICK_ADDR: u32 = 0x023C_1200;
const GAME_TICK_PROBE_GAME_FRAME_ADDR: u32 = 0x0208_B668;
const GAME_TICK_PROBE_STAGE_MARKER_ADDR: u32 = 0x04FF_FA28;
const GAME_TICK_PROBE_REPLAY_RENDER_ADDR: u32 = 0x04FF_FA2C;
const EIGHT_COIN_SFX_HOOK_ADDR: u32 = 0x020B_F370;
const EIGHT_COIN_SFX_STUB_ADDR: u32 = 0x020C_5300;
const EIGHT_COIN_SFX_ID: u32 = 0x017E;

#[derive(Debug, Clone)]
pub struct StableRomOptions {
    pub source_rom: PathBuf,
    pub host_rom: PathBuf,
    pub client_rom: PathBuf,
    pub stage: u8,
    pub wins: u8,
    pub big_stars: u8,
    pub lives: String,
    pub course_mode: String,
    pub scene_settings: Option<String>,
    pub symbols: PathBuf,
    pub game_tick_probe: bool,
}

#[derive(Debug)]
pub struct StableRomResult {
    pub host_rom: PathBuf,
    pub client_rom: PathBuf,
}

#[derive(Clone)]
struct RomImage {
    header: Vec<u8>,
    arm9_ram: u32,
    arm9_entry: u32,
    arm9_autoload_callback: u32,
    arm9_code_settings_pointer: u32,
    arm9_footer: [u8; ARM9_FOOTER_SIZE],
    arm9: Vec<u8>,
    arm9_sections: Vec<Arm9Section>,
    arm7: Vec<u8>,
    arm7_ram: u32,
    arm7_entry: u32,
    arm7_autoload_callback: u32,
    fnt: Vec<u8>,
    files: Vec<Vec<u8>>,
    overlays: Vec<Overlay>,
    arm7_overlay_table: Vec<u8>,
    banner: Vec<u8>,
    original_len: usize,
}

#[derive(Clone)]
struct Arm9Section {
    ram_addr: u32,
    file_off: usize,
    size: usize,
}

#[derive(Clone)]
struct Overlay {
    id: u32,
    base_addr: u32,
    code_size: u32,
    bss_size: u32,
    ctor_start: u32,
    ctor_end: u32,
    file_id: u32,
    flags: u32,
    data: Vec<u8>,
}

struct DirectMvlConfig {
    stage: u8,
    player_id: u8,
    scene_settings: u32,
    initial_lives: u32,
    life_mode_selector: u32,
    big_star_selector: u32,
}

pub fn generate_stable_roms(options: &StableRomOptions) -> Result<StableRomResult> {
    if !options.source_rom.exists() {
        bail!("source ROM not found: {}", options.source_rom.display());
    }
    validate_game_settings(
        options.wins,
        options.big_stars,
        &options.lives,
        &options.course_mode,
    )?;
    let scene_settings = match &options.scene_settings {
        Some(value) if !value.trim().is_empty() => parse_u32(value)?,
        _ => stage_scene_settings(options.stage)?,
    };
    let initial_lives = initial_lives(&options.lives)?;
    let life_mode_selector = life_mode_selector(&options.lives)?;
    let big_star_selector = big_star_selector(options.big_stars)?;
    let symbols = load_symbols(&options.symbols)?;
    let base = RomImage::load(&options.source_rom)?;

    let mut host = base.clone();
    let host_config = DirectMvlConfig {
        stage: options.stage,
        player_id: 0,
        scene_settings,
        initial_lives,
        life_mode_selector,
        big_star_selector,
    };
    patch_direct_mvl_entry(&mut host, &symbols, &host_config)?;
    patch_wifi_communicating_consoles(&mut host, 2)?;
    if options.game_tick_probe {
        patch_game_tick_probe(&mut host)?;
    }
    host.save(&options.host_rom)?;

    let mut client = base;
    let client_config = DirectMvlConfig {
        stage: options.stage,
        player_id: 1,
        scene_settings,
        initial_lives,
        life_mode_selector,
        big_star_selector,
    };
    patch_direct_mvl_entry(&mut client, &symbols, &client_config)?;
    patch_wifi_communicating_consoles(&mut client, 2)?;
    if options.game_tick_probe {
        patch_game_tick_probe(&mut client)?;
    }
    client.save(&options.client_rom)?;

    Ok(StableRomResult {
        host_rom: options.host_rom.clone(),
        client_rom: options.client_rom.clone(),
    })
}

fn validate_game_settings(wins: u8, big_stars: u8, lives: &str, course_mode: &str) -> Result<()> {
    if !(1..=3).contains(&wins) {
        bail!("wins must be 1, 2, or 3: {wins}");
    }
    match big_stars {
        3 | 5 | 10 => {}
        _ => bail!("big stars must be 3, 5, or 10: {big_stars}"),
    }
    match lives.to_ascii_lowercase().as_str() {
        "3" | "5" | "endless" => {}
        _ => bail!("lives must be 3, 5, or endless: {lives}"),
    }
    match course_mode.to_ascii_lowercase().as_str() {
        "random" | "select" => {}
        _ => bail!("course mode must be random or select: {course_mode}"),
    }
    Ok(())
}

pub fn stage_scene_settings(stage: u8) -> Result<u32> {
    if stage > 4 {
        bail!("stage must be between 0 and 4: {stage}");
    }
    Ok(((0xb4u32 + stage as u32) << 16) | 0xff00)
}

fn initial_lives(lives: &str) -> Result<u32> {
    match lives.to_ascii_lowercase().as_str() {
        "3" => Ok(3),
        "5" => Ok(5),
        "endless" => Ok(3),
        _ => bail!("lives must be 3, 5, or endless: {lives}"),
    }
}

fn life_mode_selector(lives: &str) -> Result<u32> {
    match lives.to_ascii_lowercase().as_str() {
        "3" | "5" => Ok(0),
        "endless" => Ok(2),
        _ => bail!("lives must be 3, 5, or endless: {lives}"),
    }
}

fn big_star_selector(big_stars: u8) -> Result<u32> {
    match big_stars {
        3 => Ok(0),
        5 => Ok(1),
        10 => Ok(2),
        _ => bail!("big stars must be 3, 5, or 10: {big_stars}"),
    }
}

impl RomImage {
    fn load(path: &Path) -> Result<Self> {
        let data = fs::read(path).with_context(|| format!("read ROM {}", path.display()))?;
        if data.len() < HEADER_SIZE {
            bail!("ROM is too small: {}", path.display());
        }

        let arm9_off = read_u32(&data, 0x20)? as usize;
        let arm9_entry = read_u32(&data, 0x24)?;
        let arm9_ram = read_u32(&data, 0x28)?;
        let arm9_size = read_u32(&data, 0x2c)? as usize;
        let arm7_off = read_u32(&data, 0x30)? as usize;
        let arm7_entry = read_u32(&data, 0x34)?;
        let arm7_ram = read_u32(&data, 0x38)?;
        let arm7_size = read_u32(&data, 0x3c)? as usize;
        let fnt_off = read_u32(&data, 0x40)? as usize;
        let fnt_size = read_u32(&data, 0x44)? as usize;
        let fat_off = read_u32(&data, 0x48)? as usize;
        let fat_size = read_u32(&data, 0x4c)? as usize;
        let arm9_ovt_off = read_u32(&data, 0x50)? as usize;
        let arm9_ovt_size = read_u32(&data, 0x54)? as usize;
        let arm7_ovt_off = read_u32(&data, 0x58)? as usize;
        let arm7_ovt_size = read_u32(&data, 0x5c)? as usize;
        let arm9_autoload_callback = read_u32(&data, 0x60)?;
        let arm9_code_settings_pointer = read_u32(&data, 0x70)?;
        let arm7_autoload_callback = read_u32(&data, 0x64)?;
        let banner_off = read_u32(&data, 0x68)? as usize;

        let arm9_raw = slice(&data, arm9_off, arm9_size, "ARM9")?;
        let arm9_footer_slice =
            slice(&data, arm9_off + arm9_size, ARM9_FOOTER_SIZE, "ARM9 footer")?;
        let mut arm9_footer = [0u8; ARM9_FOOTER_SIZE];
        arm9_footer.copy_from_slice(arm9_footer_slice);
        let arm9 = Lz77 {}
            .decompress(arm9_raw)
            .map(|v| v.into_vec())
            .context("decompress ARM9")?;
        let arm9_sections = parse_arm9_sections(&arm9, arm9_ram, arm9_code_settings_pointer)?;

        let file_count = fat_size / 8;
        let mut files = Vec::with_capacity(file_count);
        for file_id in 0..file_count {
            let entry = fat_off + file_id * 8;
            let start = read_u32(&data, entry)? as usize;
            let end = read_u32(&data, entry + 4)? as usize;
            files.push(slice(&data, start, end.saturating_sub(start), "file")?.to_vec());
        }

        let mut overlays = Vec::new();
        for off in (arm9_ovt_off..arm9_ovt_off + arm9_ovt_size).step_by(0x20) {
            let file_id = read_u32(&data, off + 0x18)?;
            let flags = read_u32(&data, off + 0x1c)?;
            let raw = files
                .get(file_id as usize)
                .ok_or_else(|| anyhow!("overlay file id out of range: {file_id}"))?;
            let compressed = (flags & (1 << 24)) != 0;
            let overlay_id = read_u32(&data, off)?;
            let overlay_data = if compressed {
                Lz77 {}
                    .decompress(raw)
                    .map(|v| v.into_vec())
                    .with_context(|| format!("decompress overlay {overlay_id}"))?
            } else {
                raw.clone()
            };
            overlays.push(Overlay {
                id: overlay_id,
                base_addr: read_u32(&data, off + 0x04)?,
                code_size: read_u32(&data, off + 0x08)?,
                bss_size: read_u32(&data, off + 0x0c)?,
                ctor_start: read_u32(&data, off + 0x10)?,
                ctor_end: read_u32(&data, off + 0x14)?,
                file_id,
                flags,
                data: overlay_data,
            });
        }

        Ok(Self {
            header: data[..HEADER_SIZE].to_vec(),
            arm9_ram,
            arm9_entry,
            arm9_autoload_callback,
            arm9_code_settings_pointer,
            arm9_footer,
            arm9,
            arm9_sections,
            arm7: slice(&data, arm7_off, arm7_size, "ARM7")?.to_vec(),
            arm7_ram,
            arm7_entry,
            arm7_autoload_callback,
            fnt: slice(&data, fnt_off, fnt_size, "FNT")?.to_vec(),
            files,
            overlays,
            arm7_overlay_table: if arm7_ovt_size == 0 {
                Vec::new()
            } else {
                slice(&data, arm7_ovt_off, arm7_ovt_size, "ARM7 overlay table")?.to_vec()
            },
            banner: read_banner(&data, banner_off)?,
            original_len: data.len(),
        })
    }

    fn save(&self, path: &Path) -> Result<()> {
        let mut output = vec![0u8; HEADER_SIZE];
        let overlay_file_ids: BTreeSet<u32> = self
            .overlays
            .iter()
            .map(|overlay| overlay.file_id)
            .collect();
        let mut fat = vec![(0u32, 0u32); self.files.len()];
        let mut arm9 = self.arm9.clone();
        let arm9_build_info = self.arm9_build_info_offset()?;

        let arm9_off = align_vec(&mut output, 0x200, 0);
        let compressed_arm9 =
            compress_arm9_with_build_info(&mut arm9, arm9_build_info, self.arm9_ram)?;
        output.extend_from_slice(&compressed_arm9);
        output.extend_from_slice(&self.arm9_footer);
        let arm9_size = compressed_arm9.len() as u32;

        let arm9_ovt_off = align_vec(&mut output, 0x200, 0);
        let mut overlay_payloads = Vec::with_capacity(self.overlays.len());
        for overlay in &self.overlays {
            overlay_payloads.push(if overlay.compressed() {
                Lz77 {}
                    .compress(&overlay.data, 0)
                    .with_context(|| format!("compress overlay {}", overlay.id))?
                    .into_vec()
            } else {
                overlay.data.clone()
            });
        }
        let mut overlay_table = Vec::with_capacity(self.overlays.len() * 0x20);
        for (overlay, data) in self.overlays.iter().zip(&overlay_payloads) {
            append_overlay_entry(&mut overlay_table, overlay, data.len() as u32);
        }
        output.extend_from_slice(&overlay_table);

        for (overlay, data) in self.overlays.iter().zip(&overlay_payloads) {
            let file_id = overlay.file_id as usize;
            let start = align_vec(&mut output, 0x200, 0);
            output.extend_from_slice(data);
            fat[file_id] = (start as u32, output.len() as u32);
        }

        let arm7_off = align_vec(&mut output, 0x200, 0);
        output.extend_from_slice(&self.arm7);

        let arm7_ovt_off = if self.arm7_overlay_table.is_empty() {
            0
        } else {
            let off = align_vec(&mut output, 0x200, 0);
            output.extend_from_slice(&self.arm7_overlay_table);
            off
        };

        let fnt_off = align_vec(&mut output, 0x200, 0);
        output.extend_from_slice(&self.fnt);

        let fat_off = align_vec(&mut output, 0x200, 0);
        let fat_size = (fat.len() * 8) as u32;
        output.resize(output.len() + fat_size as usize, 0);

        let banner_off = align_vec(&mut output, 0x200, 0);
        output.extend_from_slice(&self.banner);

        for (file_id, fat_entry) in fat.iter_mut().enumerate().take(self.files.len()) {
            if overlay_file_ids.contains(&(file_id as u32)) {
                continue;
            }
            let start = align_vec(&mut output, 0x200, 0xff);
            output.extend_from_slice(&self.files[file_id]);
            *fat_entry = (start as u32, output.len() as u32);
        }

        let rom_size = output.len() as u32;
        let padded_len = output
            .len()
            .next_power_of_two()
            .max(self.original_len.next_power_of_two());
        output.resize(padded_len, 0xff);

        for (i, (start, end)) in fat.iter().enumerate() {
            write_u32(&mut output, fat_off + i * 8, *start)?;
            write_u32(&mut output, fat_off + i * 8 + 4, *end)?;
        }

        output[..HEADER_SIZE].copy_from_slice(&self.header);
        write_u32(&mut output, 0x20, arm9_off as u32)?;
        write_u32(&mut output, 0x24, self.arm9_entry)?;
        write_u32(&mut output, 0x28, self.arm9_ram)?;
        write_u32(&mut output, 0x2c, arm9_size)?;
        write_u32(&mut output, 0x30, arm7_off as u32)?;
        write_u32(&mut output, 0x34, self.arm7_entry)?;
        write_u32(&mut output, 0x38, self.arm7_ram)?;
        write_u32(&mut output, 0x3c, self.arm7.len() as u32)?;
        write_u32(&mut output, 0x40, fnt_off as u32)?;
        write_u32(&mut output, 0x44, self.fnt.len() as u32)?;
        write_u32(&mut output, 0x48, fat_off as u32)?;
        write_u32(&mut output, 0x4c, fat_size)?;
        write_u32(&mut output, 0x50, arm9_ovt_off as u32)?;
        write_u32(&mut output, 0x54, overlay_table.len() as u32)?;
        write_u32(&mut output, 0x58, arm7_ovt_off as u32)?;
        write_u32(&mut output, 0x5c, self.arm7_overlay_table.len() as u32)?;
        write_u32(&mut output, 0x60, self.arm9_autoload_callback)?;
        write_u32(&mut output, 0x64, self.arm7_autoload_callback)?;
        write_u32(&mut output, 0x68, banner_off as u32)?;
        write_u32(&mut output, 0x70, self.arm9_code_settings_pointer)?;
        write_u32(&mut output, 0x80, rom_size)?;
        let header_crc = CRC_16_MODBUS.checksum(&output[..0x15e]);
        write_u16(&mut output, 0x15e, header_crc)?;

        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).with_context(|| format!("create {}", parent.display()))?;
        }
        fs::write(path, output).with_context(|| format!("write ROM {}", path.display()))
    }

    fn arm9_build_info_offset(&self) -> Result<usize> {
        let offset = read_u32(&self.arm9_footer, 4)? as usize;
        if offset + 0x24 > self.arm9.len() {
            bail!("ARM9 build info offset out of range: 0x{offset:x}");
        }
        Ok(offset)
    }
}

fn parse_arm9_sections(
    data: &[u8],
    ram_addr: u32,
    code_settings_pointer_addr: u32,
) -> Result<Vec<Arm9Section>> {
    let code_settings_pointer_off = code_settings_pointer_addr
        .checked_sub(ram_addr + 4)
        .ok_or_else(|| {
            anyhow!("ARM9 code settings pointer before base: 0x{code_settings_pointer_addr:08x}")
        })? as usize;
    let Ok(code_settings_addr) = read_u32(data, code_settings_pointer_off) else {
        return Ok(vec![Arm9Section {
            ram_addr,
            file_off: 0,
            size: data.len(),
        }]);
    };
    let code_settings_off = match code_settings_addr.checked_sub(ram_addr) {
        Some(off) if off as usize + 12 <= data.len() => off as usize,
        _ => {
            return Ok(vec![Arm9Section {
                ram_addr,
                file_off: 0,
                size: data.len(),
            }])
        }
    };

    let copy_table_begin = read_u32(data, code_settings_off)?.saturating_sub(ram_addr) as usize;
    let copy_table_end = read_u32(data, code_settings_off + 4)?.saturating_sub(ram_addr) as usize;
    let mut data_begin = read_u32(data, code_settings_off + 8)?.saturating_sub(ram_addr) as usize;
    if copy_table_begin > copy_table_end || copy_table_end > data.len() || data_begin > data.len() {
        return Ok(vec![Arm9Section {
            ram_addr,
            file_off: 0,
            size: data.len(),
        }]);
    }

    let mut sections = vec![Arm9Section {
        ram_addr,
        file_off: 0,
        size: data_begin,
    }];
    for entry in (copy_table_begin..copy_table_end).step_by(12) {
        if entry + 12 > data.len() {
            bail!("ARM9 copy table entry out of range: 0x{entry:x}");
        }
        let section_ram = read_u32(data, entry)?;
        let section_size = read_u32(data, entry + 4)? as usize;
        if data_begin + section_size > data.len() {
            bail!("ARM9 section out of range: off=0x{data_begin:x} size=0x{section_size:x}");
        }
        sections.push(Arm9Section {
            ram_addr: section_ram,
            file_off: data_begin,
            size: section_size,
        });
        data_begin += section_size;
    }
    Ok(sections)
}

fn compress_arm9_with_build_info(
    arm9: &mut [u8],
    build_info: usize,
    ram_base: u32,
) -> Result<Vec<u8>> {
    let mut compressed = Lz77 {}
        .compress(arm9, ARM9_COMPRESSION_START)
        .context("compress ARM9")?
        .into_vec();
    for _ in 0..3 {
        let end = ram_base + compressed.len() as u32;
        if read_u32(arm9, build_info + 0x14)? == end {
            return Ok(compressed);
        }
        write_u32(arm9, build_info + 0x14, end)?;
        compressed = Lz77 {}
            .compress(arm9, ARM9_COMPRESSION_START)
            .context("compress ARM9 after build-info update")?
            .into_vec();
    }
    Ok(compressed)
}

fn patch_direct_mvl_entry(
    rom: &mut RomImage,
    symbols: &BTreeMap<String, u32>,
    config: &DirectMvlConfig,
) -> Result<()> {
    patch_arm9_words(rom, 0x0201_3428, &[encode_mov_imm(12, 6)?])?;

    patch_overlay_words(
        rom,
        0x0215_9348,
        &[symbol(symbols, "_ZN14VSConnectScene10loadGameSME")?],
    )?;

    let update_addr = symbol(symbols, "_ZN14VSConnectScene16updateLoadGameSMEv")?;
    let stub = build_direct_loadlevel_stub(
        update_addr,
        symbol(symbols, "_ZN4Game9loadLevelEtmhhhhhhhhhhhhhhm")?,
        symbol(symbols, "_ZN14VSConnectScene19loadMvsLFilesThreadEv")?,
        config,
    )?;
    patch_overlay_words(rom, update_addr, &stub)?;

    patch_overlay_words(rom, 0x0215_2888, &[NOP])?;
    patch_arm9_words(
        rom,
        symbol(symbols, "_ZN3Net4Core14transferPacketENS_12PacketActionE")?,
        &[encode_mov_imm(0, 8)?, BX_LR],
    )?;
    patch_overlay_words(rom, 0x0215_2E64, &[encode_mov_imm(1, 0)?])?;
    patch_mvl_load_thread_entrance_ids(rom)?;
    patch_is_out_of_view_vertical_camera_fallback(rom)?;
    patch_eight_coin_reward_positional_sfx(rom, symbols)?;
    patch_camera_focus_loop_count(rom, 2)?;
    // Disabled pending a focused lifecycle proof. The original session found this
    // hook targeted 0x0209B040, while the missing-Goomba path used 0x0209B320.
    // patch_stage_object_activation_player_id(rom, 0)?;
    patch_player_stage_lock_vsmode_noop(rom)?;
    patch_player_powerup_state_allows_movement_mvl(rom)?;
    Ok(())
}

fn build_eight_coin_reward_positional_sfx_stub(
    start_addr: u32,
    get_player_addr: u32,
    play_sfx_addr: u32,
) -> Result<Vec<u32>> {
    let literal_addr = start_addr + 0x1c;
    Ok(vec![
        encode_push((1 << 4) | (1 << 14)), // push {r4, lr}; preserve stack alignment
        encode_mov_reg(0, 5),              // mov r0, r5; reward player ID
        encode_bl(start_addr + 0x08, get_player_addr)?,
        encode_add_imm(1, 0, 0x5c)?, // add r1, r0, #0x5c; player world Vec3
        encode_ldr_pc_literal(0, start_addr + 0x10, literal_addr, 0xE)?,
        encode_bl(start_addr + 0x14, play_sfx_addr)?,
        POP_PC | (1 << 4),
        EIGHT_COIN_SFX_ID,
    ])
}

fn patch_eight_coin_reward_positional_sfx(
    rom: &mut RomImage,
    symbols: &BTreeMap<String, u32>,
) -> Result<()> {
    let get_player_addr = symbol(symbols, "_ZN4Game9getPlayerEl")?;
    let play_sfx_addr = symbol(symbols, "_ZN3SND7playSFXElPK4Vec3")?;
    let stub = build_eight_coin_reward_positional_sfx_stub(
        EIGHT_COIN_SFX_STUB_ADDR,
        get_player_addr,
        play_sfx_addr,
    )?;
    ensure_zero_overlay_words(rom, 0, EIGHT_COIN_SFX_STUB_ADDR, stub.len())?;
    patch_overlay_words_by_id(rom, 0, EIGHT_COIN_SFX_STUB_ADDR, &stub)?;

    let expected = [
        0xE59F_0050, // ldr r0, [pc, #0x50]; SFX ID 0x17e
        encode_mov_imm(1, 0)?,
        encode_bl(EIGHT_COIN_SFX_HOOK_ADDR + 0x08, play_sfx_addr)?,
    ];
    let replacement = [
        encode_bl(EIGHT_COIN_SFX_HOOK_ADDR, EIGHT_COIN_SFX_STUB_ADDR)?,
        NOP,
        NOP,
    ];
    let old = patch_overlay_words_by_id(rom, 0, EIGHT_COIN_SFX_HOOK_ADDR, &replacement)?;
    let old_words = old
        .chunks_exact(4)
        .map(|word| u32::from_le_bytes(word.try_into().expect("four-byte ARM word")))
        .collect::<Vec<_>>();
    if old_words != expected {
        bail!(
            "8-coin SFX hook @ 0x{EIGHT_COIN_SFX_HOOK_ADDR:08x} expected {expected:08x?}, got {old_words:08x?}"
        );
    }
    Ok(())
}

fn build_direct_loadlevel_stub(
    start_addr: u32,
    load_level_addr: u32,
    load_mvl_files_after_addr: u32,
    config: &DirectMvlConfig,
) -> Result<Vec<u32>> {
    let stack_values = [
        0,                       // act
        config.player_id as u32, // playerID
        3,                       // playerMask
        0,                       // character1
        1,                       // character2
        0,                       // powerup
        0xff,                    // entrance
        1,                       // flag
        1,                       // unused/control flag observed in VSConnect
        0xff,                    // controlOptions observed in normal MvL load path
        0,                       // unused2
        0,                       // challengeMode
        0xffff_ffff,             // rngSeed: use network/random state
    ];

    let mut words = vec![
        encode_push((1 << 4) | (1 << 14)),
        encode_mov_reg(4, 0),
        encode_ldr_imm(12, 4, 0x16c)?,
        encode_cmp_imm(12, 0x77)?,
        with_cond(encode_mov_imm(0, 1)?, 0),
        with_cond(POP_PC | (1 << 4), 0),
        encode_mov_imm(12, 0x77)?,
        encode_str_imm(12, 4, 0x16c)?,
        encode_sub_sp_imm(0x38)?,
    ];

    let mut literals: Vec<u32> = Vec::new();
    let mut literal_refs: Vec<(usize, u8, usize, u8)> = Vec::new();
    let mut emit_ldr_literal = |words: &mut Vec<u32>, rd: u8, value: u32, cond: u8| {
        let literal_index = literals.len();
        literals.push(value);
        let word_index = words.len();
        words.push(0);
        literal_refs.push((word_index, rd, literal_index, cond));
    };

    emit_ldr_literal(&mut words, 4, MVL_RUNTIME_CONFIG_ADDR, 0xE);
    words.push(encode_ldr_imm(12, 4, 0)?);
    emit_ldr_literal(&mut words, 0, MVL_RUNTIME_CONFIG_MAGIC, 0xE);
    words.push(encode_cmp_reg(12, 0));
    words.push(encode_mov_imm(0, 0x0f)?);
    words.push(encode_mov_imm(1, 1)?);
    words.push(encode_mov_imm(2, 9)?);
    words.push(encode_mov_imm(3, config.stage as u32)?);
    words.push(with_cond(
        encode_ldr_imm(3, 4, MVL_RUNTIME_CONFIG_STAGE_OFFSET)?,
        0,
    ));

    let mut current_ip_value = None;
    for (i, value) in stack_values.iter().enumerate() {
        if current_ip_value != Some(*value) {
            words.push(encode_load_imm(12, *value)?);
            current_ip_value = Some(*value);
        }
        words.push(encode_str_imm(12, 13, (i * 4) as u32)?);
    }

    let bl_addr = start_addr + words.len() as u32 * 4;
    words.push(encode_bl(bl_addr, load_level_addr)?);
    let bl_load_files_addr = start_addr + words.len() as u32 * 4;
    words.push(encode_bl(bl_load_files_addr, load_mvl_files_after_addr)?);

    words.push(encode_ldr_imm(12, 4, 0)?);
    emit_ldr_literal(&mut words, 2, MVL_RUNTIME_CONFIG_MAGIC, 0xE);
    words.push(encode_cmp_reg(12, 2));

    emit_ldr_literal(&mut words, 0, 0x0208_8F38, 0xE);
    emit_ldr_literal(&mut words, 1, config.scene_settings, 0xE);
    words.push(with_cond(
        encode_ldr_imm(1, 4, MVL_RUNTIME_CONFIG_SCENE_SETTINGS_OFFSET)?,
        0,
    ));
    words.push(encode_str_imm(1, 0, 0)?);

    emit_ldr_literal(&mut words, 0, MVL_NATIVE_COURSE_SELECTOR_ADDR, 0xE);
    words.push(encode_mov_imm(1, config.stage as u32)?);
    words.push(with_cond(
        encode_ldr_imm(1, 4, MVL_RUNTIME_CONFIG_STAGE_OFFSET)?,
        0,
    ));
    words.push(encode_strb_imm(1, 0, 0)?);

    emit_ldr_literal(&mut words, 0, 0x0208_B364, 0xE);
    words.push(encode_load_imm(1, config.initial_lives)?);
    words.push(with_cond(
        encode_ldr_imm(1, 4, MVL_RUNTIME_CONFIG_INITIAL_LIVES_OFFSET)?,
        0,
    ));
    words.push(encode_str_imm(1, 0, 0)?);
    words.push(encode_str_imm(1, 0, 4)?);

    emit_ldr_literal(&mut words, 0, 0x0215_C89C, 0xE);
    words.push(encode_mov_imm(1, config.life_mode_selector)?);
    words.push(with_cond(
        encode_ldr_imm(1, 4, MVL_RUNTIME_CONFIG_LIFE_MODE_SELECTOR_OFFSET)?,
        0,
    ));
    words.push(encode_strb_imm(1, 0, 0)?);

    emit_ldr_literal(&mut words, 0, 0x0215_C88C, 0xE);
    words.push(encode_mov_imm(1, config.big_star_selector)?);
    words.push(with_cond(
        encode_ldr_imm(1, 4, MVL_RUNTIME_CONFIG_BIG_STAR_SELECTOR_OFFSET)?,
        0,
    ));
    words.push(encode_strb_imm(1, 0, 0)?);

    emit_ldr_literal(&mut words, 0, 0x0208_B094, 0xE);
    words.push(encode_mov_imm(1, 0)?);
    words.push(encode_strb_imm(1, 0, 0)?);
    words.push(encode_mov_imm(1, 1)?);
    words.push(encode_strb_imm(1, 0, 1)?);
    emit_ldr_literal(&mut words, 0, 0x0208_B098, 0xE);
    words.push(encode_mov_imm(1, 0)?);
    words.push(encode_strb_imm(1, 0, 0)?);
    words.push(encode_strb_imm(1, 0, 1)?);
    emit_ldr_literal(&mut words, 0, GAME_PLAYER_INVENTORY_POWERUP_ADDR, 0xE);
    words.push(encode_mov_imm(1, 0)?);
    words.push(encode_strb_imm(1, 0, 0)?);
    words.push(encode_strb_imm(1, 0, 1)?);
    emit_ldr_literal(&mut words, 0, 0x0208_B0A0, 0xE);
    words.push(encode_ldr_imm(1, 0, 0)?);
    words.push(encode_add_imm(2, 1, 0x14)?);
    words.push(encode_str_imm(2, 0, 4)?);
    emit_ldr_literal(&mut words, 0, 0x0208_87F0, 0xE);
    words.push(encode_mov_imm(1, (config.player_id & 3) as u32)?);
    words.push(encode_str_imm(1, 0, 0)?);
    words.push(encode_add_sp_imm(0x38)?);
    words.push(encode_mov_imm(0, 1)?);
    words.push(POP_PC | (1 << 4));

    let literal_start_addr = start_addr + words.len() as u32 * 4;
    for (word_index, rd, literal_index, cond) in literal_refs {
        let instruction_addr = start_addr + word_index as u32 * 4;
        let literal_addr = literal_start_addr + literal_index as u32 * 4;
        words[word_index] = encode_ldr_pc_literal(rd, instruction_addr, literal_addr, cond)?;
    }
    words.extend(literals);
    Ok(words)
}

fn patch_wifi_communicating_consoles(rom: &mut RomImage, count: u8) -> Result<()> {
    if !(1..=4).contains(&count) {
        bail!("communicating console count must be 1..4, got {count}");
    }
    patch_arm9_words(rom, 0x0204_6C34, &[encode_mov_imm(0, count as u32)?, BX_LR])?;
    patch_arm9_words(
        rom,
        0x0204_6C44,
        &[
            encode_cmp_imm(0, count as u32)?,
            with_cond(encode_mov_imm(0, 1)?, 3),
            with_cond(encode_mov_imm(0, 0)?, 2),
            BX_LR,
        ],
    )?;
    Ok(())
}

fn patch_game_tick_probe(rom: &mut RomImage) -> Result<()> {
    const LOOP_START_ADDR: u32 = 0x0200_4EC8;
    const FONT_HOOK_ADDR: u32 = 0x0200_4EEC;
    const COUNTER_TAIL_HOOK_ADDR: u32 = 0x0200_4EFC;
    const COUNTER_TAIL_RETURN_ADDR: u32 = 0x0200_4F00;
    const RENDER_HOOK_ADDR: u32 = 0x0204_D5EC;
    const RENDER_RETURN_ADDR: u32 = 0x0204_D5F0;
    const FONT_UPDATE_ADDR: u32 = 0x0201_4A44;
    const PROCESS_LIST_EXECUTE_ADDR: u32 = 0x0204_D46C;
    const OS_TICK_ADDR: u32 = 0x01FF_9010;
    const LOOP_GATE_ADDR: u32 = 0x0200_19C0;
    const FONT_GATE_ADDR: u32 = 0x0200_1A20;
    const RENDER_GATE_ADDR: u32 = 0x0200_1A60;
    const INPUT_GATE_ADDR: u32 = 0x0200_1B40;
    const PROCESS_STAGE_GATE_ADDR: u32 = 0x0200_1D00;
    const INPUT_UPDATE_ADDR: u32 = 0x0200_5230;

    let loop_gate = [
        encode_ldr_pc_literal(3, LOOP_GATE_ADDR, LOOP_GATE_ADDR + 0x58, 0xE)?,
        encode_mov_imm(12, 6)?,
        encode_str_imm(12, 3, 0)?,
        encode_ldr_pc_literal(3, LOOP_GATE_ADDR + 0x0C, LOOP_GATE_ADDR + 0x4C, 0xE)?,
        encode_ldr_imm(12, 3, 0)?,
        encode_cmp_imm(12, 0)?,
        with_cond(encode_b(LOOP_GATE_ADDR + 0x18, LOOP_GATE_ADDR + 0x34)?, 0),
        encode_sub_imm(12, 12, 1)?,
        encode_str_imm(12, 3, 0)?,
        encode_ldr_pc_literal(3, LOOP_GATE_ADDR + 0x24, LOOP_GATE_ADDR + 0x50, 0xE)?,
        encode_mov_imm(12, 1)?,
        encode_str_imm(12, 3, 0)?,
        encode_b(LOOP_GATE_ADDR + 0x30, LOOP_START_ADDR)?,
        encode_ldr_pc_literal(3, LOOP_GATE_ADDR + 0x34, LOOP_GATE_ADDR + 0x50, 0xE)?,
        encode_mov_imm(12, 0)?,
        encode_str_imm(12, 3, 0)?,
        encode_bl(LOOP_GATE_ADDR + 0x40, OS_TICK_ADDR)?,
        encode_ldr_pc_literal(14, LOOP_GATE_ADDR + 0x44, LOOP_GATE_ADDR + 0x54, 0xE)?,
        encode_b(LOOP_GATE_ADDR + 0x48, COUNTER_TAIL_RETURN_ADDR)?,
        GAME_TICK_PROBE_REQUEST_ADDR,
        GAME_TICK_PROBE_ACTIVE_ADDR,
        COUNTER_TAIL_RETURN_ADDR,
        GAME_TICK_PROBE_STAGE_MARKER_ADDR,
    ];
    let font_gate = [
        encode_ldr_pc_literal(12, FONT_GATE_ADDR, FONT_GATE_ADDR + 0x1C, 0xE)?,
        encode_ldr_imm(12, 12, 0)?,
        encode_cmp_imm(12, 0)?,
        with_cond(encode_b(FONT_GATE_ADDR + 0x0C, FONT_GATE_ADDR + 0x14)?, 1),
        encode_bl(FONT_GATE_ADDR + 0x10, FONT_UPDATE_ADDR)?,
        encode_ldr_pc_literal(14, FONT_GATE_ADDR + 0x14, FONT_GATE_ADDR + 0x20, 0xE)?,
        encode_b(FONT_GATE_ADDR + 0x18, FONT_HOOK_ADDR + 4)?,
        GAME_TICK_PROBE_ACTIVE_ADDR,
        FONT_HOOK_ADDR + 4,
    ];
    let render_gate = [
        encode_ldr_pc_literal(3, RENDER_GATE_ADDR, RENDER_GATE_ADDR + 0x44, 0xE)?,
        encode_mov_imm(12, 4)?,
        encode_str_imm(12, 3, 0)?,
        encode_ldr_pc_literal(12, RENDER_GATE_ADDR + 0x0C, RENDER_GATE_ADDR + 0x48, 0xE)?,
        encode_ldr_imm(12, 12, 0)?,
        encode_cmp_imm(12, 0)?,
        with_cond(
            encode_b(RENDER_GATE_ADDR + 0x18, RENDER_GATE_ADDR + 0x2C)?,
            0,
        ),
        encode_ldr_pc_literal(12, RENDER_GATE_ADDR + 0x1C, RENDER_GATE_ADDR + 0x4C, 0xE)?,
        encode_ldr_imm(12, 12, 0)?,
        encode_cmp_imm(12, 0)?,
        with_cond(
            encode_b(RENDER_GATE_ADDR + 0x28, RENDER_GATE_ADDR + 0x30)?,
            0,
        ),
        encode_bl(RENDER_GATE_ADDR + 0x2C, PROCESS_LIST_EXECUTE_ADDR)?,
        encode_ldr_pc_literal(3, RENDER_GATE_ADDR + 0x30, RENDER_GATE_ADDR + 0x44, 0xE)?,
        encode_mov_imm(12, 5)?,
        encode_str_imm(12, 3, 0)?,
        encode_ldr_pc_literal(14, RENDER_GATE_ADDR + 0x3C, RENDER_GATE_ADDR + 0x50, 0xE)?,
        encode_b(RENDER_GATE_ADDR + 0x40, RENDER_RETURN_ADDR)?,
        GAME_TICK_PROBE_STAGE_MARKER_ADDR,
        GAME_TICK_PROBE_ACTIVE_ADDR,
        GAME_TICK_PROBE_REPLAY_RENDER_ADDR,
        RENDER_RETURN_ADDR,
    ];
    let input_gate = build_game_tick_input_gate(INPUT_GATE_ADDR, INPUT_UPDATE_ADDR)?;
    let process_stage_gate = build_game_tick_process_stage_gate(PROCESS_STAGE_GATE_ADDR)?;
    let input_gate_end = INPUT_GATE_ADDR + input_gate.len() as u32 * 4;
    if input_gate_end > PROCESS_STAGE_GATE_ADDR {
        bail!(
            "game-tick input gate overlaps process-stage gate: end=0x{input_gate_end:08x} limit=0x{PROCESS_STAGE_GATE_ADDR:08x}"
        );
    }

    ensure_zero_arm9_words(rom, LOOP_GATE_ADDR, loop_gate.len())?;
    ensure_zero_arm9_words(rom, FONT_GATE_ADDR, font_gate.len())?;
    ensure_zero_arm9_words(rom, RENDER_GATE_ADDR, render_gate.len())?;
    ensure_zero_arm9_words(rom, INPUT_GATE_ADDR, input_gate.len())?;
    ensure_zero_arm9_words(rom, PROCESS_STAGE_GATE_ADDR, process_stage_gate.len())?;
    ensure_zero_arm9_words(rom, GAME_TICK_PROBE_REQUEST_ADDR, 2)?;
    ensure_zero_arm9_words(rom, GAME_TICK_PROBE_MAGIC_ADDR, 1)?;
    ensure_zero_arm9_words(rom, GAME_TICK_PROBE_HISTORY_ENABLED_ADDR, 5)?;
    patch_arm9_words(rom, LOOP_GATE_ADDR, &loop_gate)?;
    patch_arm9_words(rom, FONT_GATE_ADDR, &font_gate)?;
    patch_arm9_words(rom, RENDER_GATE_ADDR, &render_gate)?;
    patch_arm9_words(rom, INPUT_GATE_ADDR, &input_gate)?;
    patch_arm9_words(rom, PROCESS_STAGE_GATE_ADDR, &process_stage_gate)?;
    patch_arm9_words(rom, GAME_TICK_PROBE_REQUEST_ADDR, &[0, 0])?;
    patch_arm9_words(rom, GAME_TICK_PROBE_MAGIC_ADDR, &[GAME_TICK_PROBE_MAGIC])?;
    patch_arm9_words(rom, GAME_TICK_PROBE_HISTORY_ENABLED_ADDR, &[0, 0, 0, 0, 0])?;

    patch_arm9_word_checked(
        rom,
        LOOP_START_ADDR,
        encode_bl(LOOP_START_ADDR, INPUT_UPDATE_ADDR)?,
        encode_b(LOOP_START_ADDR, INPUT_GATE_ADDR)?,
        "game-tick input-history gate",
    )?;

    patch_arm9_word_checked(
        rom,
        FONT_HOOK_ADDR,
        encode_bl(FONT_HOOK_ADDR, FONT_UPDATE_ADDR)?,
        encode_b(FONT_HOOK_ADDR, FONT_GATE_ADDR)?,
        "game-tick font gate",
    )?;
    patch_arm9_word_checked(
        rom,
        COUNTER_TAIL_HOOK_ADDR,
        encode_bl(COUNTER_TAIL_HOOK_ADDR, OS_TICK_ADDR)?,
        encode_b(COUNTER_TAIL_HOOK_ADDR, LOOP_GATE_ADDR)?,
        "game-tick loop gate",
    )?;
    patch_arm9_word_checked(
        rom,
        RENDER_HOOK_ADDR,
        encode_bl(RENDER_HOOK_ADDR, PROCESS_LIST_EXECUTE_ADDR)?,
        encode_b(RENDER_HOOK_ADDR, RENDER_GATE_ADDR)?,
        "game-tick render gate",
    )?;
    for (hook_addr, expected) in [
        (0x0204_D5B0, 0xEBFF_FFAD),
        (0x0204_D5C4, 0xEBFF_FFA8),
        (0x0204_D5D8, 0xEBFF_FFA3),
    ] {
        patch_arm9_word_checked(
            rom,
            hook_addr,
            expected,
            encode_bl(hook_addr, PROCESS_STAGE_GATE_ADDR)?,
            "game-tick process-list stage gate",
        )?;
    }
    Ok(())
}

fn build_game_tick_process_stage_gate(start_addr: u32) -> Result<Vec<u32>> {
    const PROCESS_LIST_EXECUTE_ADDR: u32 = 0x0204_D46C;
    const DELETE_RETURN_ADDR: u32 = 0x0204_D5B4;
    const CREATE_RETURN_ADDR: u32 = 0x0204_D5C8;
    const UPDATE_RETURN_ADDR: u32 = 0x0204_D5DC;

    let mut words = Vec::new();
    let mut literals = Vec::new();
    let mut refs: Vec<(usize, u8, usize)> = Vec::new();
    let mut emit = |words: &mut Vec<u32>, rd: u8, value: u32| {
        let literal_index = literals.len();
        literals.push(value);
        let word_index = words.len();
        words.push(0);
        refs.push((word_index, rd, literal_index));
    };

    // Six saved registers keep the stack 8-byte aligned across the nested call.
    words.push(encode_push(
        (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 12) | (1 << 14),
    ));
    emit(&mut words, 1, GAME_TICK_PROBE_STAGE_MARKER_ADDR);
    words.push(encode_ldr_imm(2, 13, 20)?);
    for (return_addr, marker) in [
        (DELETE_RETURN_ADDR, 7),
        (CREATE_RETURN_ADDR, 9),
        (UPDATE_RETURN_ADDR, 11),
    ] {
        emit(&mut words, 3, return_addr);
        words.push(encode_cmp_reg(2, 3));
        words.push(with_cond(encode_mov_imm(2, marker)?, 0));
    }
    words.push(encode_str_imm(2, 1, 0)?);
    words.push(encode_ldr_imm(0, 13, 0)?);
    let call_index = words.len();
    words.push(encode_bl(
        start_addr + call_index as u32 * 4,
        PROCESS_LIST_EXECUTE_ADDR,
    )?);

    emit(&mut words, 1, GAME_TICK_PROBE_STAGE_MARKER_ADDR);
    words.push(encode_ldr_imm(2, 13, 20)?);
    for (return_addr, marker) in [
        (DELETE_RETURN_ADDR, 8),
        (CREATE_RETURN_ADDR, 10),
        (UPDATE_RETURN_ADDR, 12),
    ] {
        emit(&mut words, 3, return_addr);
        words.push(encode_cmp_reg(2, 3));
        words.push(with_cond(encode_mov_imm(2, marker)?, 0));
    }
    words.push(encode_str_imm(2, 1, 0)?);
    words.push(0xE8BD_900F); // pop {r0-r3, r12, pc}

    let literal_start_addr = start_addr + words.len() as u32 * 4;
    for (word_index, rd, literal_index) in refs {
        let instruction_addr = start_addr + word_index as u32 * 4;
        let literal_addr = literal_start_addr + literal_index as u32 * 4;
        words[word_index] = encode_ldr_pc_literal(rd, instruction_addr, literal_addr, 0xE)?;
    }
    words.extend(literals);
    Ok(words)
}

fn build_game_tick_input_gate(start_addr: u32, input_update_addr: u32) -> Result<Vec<u32>> {
    if GAME_TICK_PROBE_HISTORY_ENTRY_BYTES != 16 {
        bail!(
            "unsupported game-tick history entry size: {}",
            GAME_TICK_PROBE_HISTORY_ENTRY_BYTES
        );
    }
    let history_bytes = GAME_TICK_PROBE_HISTORY_CAPACITY
        .checked_mul(GAME_TICK_PROBE_HISTORY_ENTRY_BYTES)
        .context("game-tick history size overflow")?;
    let history_end = GAME_TICK_PROBE_HISTORY_ADDR
        .checked_add(history_bytes as u32)
        .context("game-tick history address overflow")?;
    if history_end > 0x0240_0000 {
        bail!("game-tick history exceeds Main RAM: end=0x{history_end:08x}");
    }
    let mut words = Vec::new();
    let mut literals = Vec::new();
    let mut refs: Vec<(usize, u8, usize, u8)> = Vec::new();
    let mut emit = |words: &mut Vec<u32>, rd: u8, value: u32| {
        let literal_index = literals.len();
        literals.push(value);
        let word_index = words.len();
        words.push(0);
        refs.push((word_index, rd, literal_index, 0xE));
    };

    emit(&mut words, 3, GAME_TICK_PROBE_HISTORY_ENABLED_ADDR);
    words.push(encode_ldr_imm(12, 3, 0)?);
    words.push(encode_cmp_imm(12, 0)?);
    let inactive_branch = words.len();
    words.push(0);
    emit(&mut words, 3, GAME_TICK_PROBE_HISTORY_INDEX_ADDR);
    words.push(encode_ldr_imm(12, 3, 0)?);
    emit(&mut words, 2, GAME_TICK_PROBE_HISTORY_COUNT_ADDR);
    words.push(encode_ldr_imm(2, 2, 0)?);
    words.push(encode_cmp_reg(12, 2));
    let exhausted_branch = words.len();
    words.push(0);
    words.push(encode_cmp_imm(12, 0)?);
    let transaction_started_branch = words.len();
    words.push(0);
    emit(&mut words, 2, GAME_TICK_PROBE_HISTORY_START_FRAME_ADDR);
    words.push(encode_ldr_imm(1, 2, 0)?);
    emit(&mut words, 2, GAME_TICK_PROBE_GAME_FRAME_ADDR);
    words.push(encode_ldr_imm(2, 2, 0)?);
    words.push(encode_cmp_reg(2, 1));
    let wrong_frame_branch = words.len();
    words.push(0);
    emit(&mut words, 2, GAME_TICK_PROBE_STAGE_MARKER_ADDR);
    words.push(encode_mov_imm(1, 1)?);
    words.push(encode_str_imm(1, 2, 0)?);
    let target_check_index = words.len();
    emit(&mut words, 2, GAME_TICK_PROBE_HISTORY_TARGET_ADDR);
    words.push(encode_ldr_imm(1, 2, 0)?);
    words.push(encode_cmp_imm(1, 0)?);
    let non_target_branch = words.len();
    words.push(0);
    words.push(encode_cmp_imm(12, 0)?);
    let already_started_branch = words.len();
    words.push(0);
    emit(&mut words, 1, GAME_TICK_PROBE_ACTIVE_ADDR);
    words.push(encode_mov_imm(0, 1)?);
    words.push(encode_str_imm(0, 1, 0)?);
    emit(&mut words, 1, GAME_TICK_PROBE_REQUEST_ADDR);
    emit(&mut words, 0, GAME_TICK_PROBE_HISTORY_COUNT_ADDR);
    words.push(encode_ldr_imm(0, 0, 0)?);
    words.push(encode_sub_imm(0, 0, 1)?);
    words.push(encode_str_imm(0, 1, 0)?);
    let final_render_check_index = words.len();
    emit(&mut words, 0, GAME_TICK_PROBE_HISTORY_TARGET_ADDR);
    words.push(encode_ldr_imm(0, 0, 0)?);
    words.push(encode_cmp_imm(0, 0)?);
    let final_render_non_target_branch = words.len();
    words.push(0);
    emit(&mut words, 0, GAME_TICK_PROBE_HISTORY_COUNT_ADDR);
    words.push(encode_ldr_imm(0, 0, 0)?);
    words.push(encode_sub_imm(0, 0, 1)?);
    words.push(encode_cmp_reg(12, 0));
    let final_render_not_last_branch = words.len();
    words.push(0);
    emit(&mut words, 0, GAME_TICK_PROBE_ACTIVE_ADDR);
    words.push(encode_mov_imm(1, 0)?);
    words.push(encode_str_imm(1, 0, 0)?);
    let history_load_index = words.len();
    emit(&mut words, 2, GAME_TICK_PROBE_STAGE_MARKER_ADDR);
    words.push(encode_mov_imm(1, 2)?);
    words.push(encode_str_imm(1, 2, 0)?);
    emit(&mut words, 2, GAME_TICK_PROBE_HISTORY_ADDR);
    words.push(encode_add_reg_lsl(2, 2, 12, 4)?);
    words.push(encode_ldrh_imm(0, 2, 0)?);
    words.push(encode_ldrh_imm(1, 2, 2)?);
    words.push(encode_ldrh_imm(12, 2, 4)?);
    // Each metadata word is the packet's action/touch/x/y bytes at +4..+7.
    // Loading and storing the full word keeps both touch transitions and
    // coordinates exact while avoiding byte-unpack instructions in the gate.
    words.push(encode_ldr_imm(3, 2, 8)?);
    words.push(encode_ldr_imm(2, 2, 12)?);
    emit(&mut words, 14, GAME_TICK_PROBE_JIT_SCRATCH_TICK_ADDR);
    words.push(encode_strh_imm(0, 14, 0)?);
    words.push(encode_strh_imm(1, 14, 8)?);
    words.push(encode_strh_imm(12, 14, 10)?);
    words.push(encode_strh_imm(0, 14, 0x40)?);
    words.push(encode_strh_imm(1, 14, 0x42)?);
    words.push(encode_str_imm(3, 14, 0x44)?);
    words.push(encode_strh_imm(0, 14, 0x80)?);
    words.push(encode_strh_imm(12, 14, 0x82)?);
    words.push(encode_str_imm(2, 14, 0x84)?);
    emit(&mut words, 2, GAME_TICK_PROBE_PACKET_TICK_ADDR);
    words.push(encode_strh_imm(0, 2, 0)?);
    emit(&mut words, 3, GAME_TICK_PROBE_HISTORY_INDEX_ADDR);
    words.push(encode_ldr_imm(2, 3, 0)?);
    words.push(encode_add_imm(2, 2, 1)?);
    words.push(encode_str_imm(2, 3, 0)?);
    let call_index = words.len();
    words.push(encode_bl(
        start_addr + call_index as u32 * 4,
        input_update_addr,
    )?);
    emit(&mut words, 2, GAME_TICK_PROBE_STAGE_MARKER_ADDR);
    words.push(encode_mov_imm(1, 3)?);
    words.push(encode_str_imm(1, 2, 0)?);
    emit(&mut words, 14, 0x0200_4ECC);
    words.push(encode_b(start_addr + words.len() as u32 * 4, 0x0200_4ECC)?);
    let spin_index = words.len();
    words.push(encode_b(start_addr + spin_index as u32 * 4, start_addr)?);

    let call_addr = start_addr + call_index as u32 * 4;
    words[inactive_branch] = with_cond(
        encode_b(start_addr + inactive_branch as u32 * 4, call_addr)?,
        0,
    );
    words[exhausted_branch] = with_cond(
        encode_b(
            start_addr + exhausted_branch as u32 * 4,
            start_addr + spin_index as u32 * 4,
        )?,
        2,
    );
    words[wrong_frame_branch] = with_cond(
        encode_b(start_addr + wrong_frame_branch as u32 * 4, call_addr)?,
        1,
    );
    words[transaction_started_branch] = with_cond(
        encode_b(
            start_addr + transaction_started_branch as u32 * 4,
            start_addr + target_check_index as u32 * 4,
        )?,
        1,
    );
    let history_load_addr = start_addr + history_load_index as u32 * 4;
    words[non_target_branch] = with_cond(
        encode_b(start_addr + non_target_branch as u32 * 4, history_load_addr)?,
        0,
    );
    words[already_started_branch] = with_cond(
        encode_b(
            start_addr + already_started_branch as u32 * 4,
            start_addr + final_render_check_index as u32 * 4,
        )?,
        1,
    );
    words[final_render_non_target_branch] = with_cond(
        encode_b(
            start_addr + final_render_non_target_branch as u32 * 4,
            history_load_addr,
        )?,
        0,
    );
    words[final_render_not_last_branch] = with_cond(
        encode_b(
            start_addr + final_render_not_last_branch as u32 * 4,
            history_load_addr,
        )?,
        1,
    );

    let literal_start_addr = start_addr + words.len() as u32 * 4;
    for (word_index, rd, literal_index, cond) in refs {
        let instruction_addr = start_addr + word_index as u32 * 4;
        let literal_addr = literal_start_addr + literal_index as u32 * 4;
        words[word_index] = encode_ldr_pc_literal(rd, instruction_addr, literal_addr, cond)?;
    }
    words.extend(literals);
    Ok(words)
}

fn patch_mvl_load_thread_entrance_ids(rom: &mut RomImage) -> Result<()> {
    for (addr, word) in [
        (0x0215_2D64, encode_mov_imm(0, 1)?),
        (0x0215_2D68, encode_strb_imm(0, 13, 0x1d)?),
        (0x0215_2D74, encode_strb_imm(0, 13, 0x1c)?),
        (0x0215_2DC0, encode_mov_imm(0, 0)?),
        (0x0215_2DC8, encode_mov_imm(0, 1)?),
        (0x0215_2E00, encode_mov_imm(0, 0)?),
        (0x0215_2E0C, encode_mov_imm(0, 1)?),
    ] {
        patch_overlay_words(rom, addr, &[word])?;
    }
    Ok(())
}

fn patch_is_out_of_view_vertical_camera_fallback(rom: &mut RomImage) -> Result<()> {
    let stub_addr = 0x020C_5298;
    let stub = build_is_out_of_view_vertical_camera_fallback_stub(stub_addr)?;
    ensure_zero_overlay_words(rom, 0, stub_addr, stub.len())?;
    patch_overlay_words_by_id(rom, 0, stub_addr, &stub)?;
    patch_overlay_words_by_id(rom, 0, 0x020A_06DC, &[encode_b(0x020A_06DC, stub_addr)?])?;
    Ok(())
}

fn build_is_out_of_view_vertical_camera_fallback_stub(start_addr: u32) -> Result<Vec<u32>> {
    let mut words = Vec::new();
    let mut literals: Vec<u32> = Vec::new();
    let mut refs: Vec<(usize, u8, usize, u8)> = Vec::new();
    let mut emit = |words: &mut Vec<u32>, rd: u8, value: u32, cond: u8| {
        let literal_index = literals.len();
        literals.push(value);
        let word_index = words.len();
        words.push(0);
        refs.push((word_index, rd, literal_index, cond));
    };

    words.push(encode_push((1 << 4) | (1 << 14)));
    emit(&mut words, 3, 0x020C_AD8C, 0xE);
    words.push(encode_ldr_reg_lsl(12, 3, 2, 2)?);
    words.push(encode_cmp_imm(12, 0)?);
    words.push(with_cond(encode_mov_imm(2, 0)?, 0));
    emit(&mut words, 12, 0x020C_AD94, 0xE);
    words.push(encode_ldr_imm(14, 1, 4)?);
    emit(&mut words, 3, 0x020C_AD8C, 0xE);
    words.push(encode_ldr_imm(4, 0, 0x64)?);
    words.push(encode_ldr_reg_lsl(12, 12, 2, 2)?);
    words.push(encode_ldr_reg_lsl(0, 3, 2, 2)?);
    words.push(encode_add_reg(2, 4, 14));
    words.push(encode_add_reg(0, 12, 0));
    words.push(encode_ldr_imm(1, 1, 0x0c)?);
    words.push(encode_add_imm(2, 2, 0x18000)?);
    words.push(encode_add_reg(1, 2, 1));
    words.push(encode_rsb_imm(0, 0, 0)?);
    words.push(0xE151_0000);
    words.push(with_cond(encode_mov_imm(0, 1)?, 0xB));
    words.push(with_cond(encode_mov_imm(0, 0)?, 0xA));
    words.push(POP_PC | (1 << 4));

    let literal_start_addr = start_addr + words.len() as u32 * 4;
    for (word_index, rd, literal_index, cond) in refs {
        let instruction_addr = start_addr + word_index as u32 * 4;
        let literal_addr = literal_start_addr + literal_index as u32 * 4;
        words[word_index] = encode_ldr_pc_literal(rd, instruction_addr, literal_addr, cond)?;
    }
    words.extend(literals);
    Ok(words)
}

fn patch_camera_focus_loop_count(rom: &mut RomImage, count: u8) -> Result<()> {
    let word = encode_mov_imm(0, count as u32)?;
    patch_overlay_words_by_id(rom, 0, 0x020B_AAE4, &[word])?;
    patch_overlay_words_by_id(rom, 0, 0x020B_AC18, &[word])?;
    Ok(())
}

#[allow(dead_code)]
fn patch_stage_object_activation_player_id(rom: &mut RomImage, player_id: u8) -> Result<()> {
    if player_id > 1 {
        bail!("stage object activation player id must be 0 or 1: {player_id}");
    }

    let hook_addr = 0x0209_B048;
    let return_addr = 0x0209_B050;
    let stub_addr = 0x020C_53D0;
    let get_player_addr = 0x0202_0608;
    let stub = [
        encode_push(1 << 14),
        encode_mov_imm(0, player_id as u32)?,
        encode_str_imm(0, 13, 0x0C)?,
        encode_bl(stub_addr + 0x0C, get_player_addr)?,
        0xE8BD_4000,
        encode_b(stub_addr + 0x14, return_addr)?,
    ];

    ensure_zero_overlay_words(rom, 0, stub_addr, stub.len())?;
    patch_overlay_words_by_id(rom, 0, stub_addr, &stub)?;
    patch_overlay_words_by_id(rom, 0, hook_addr, &[encode_b(hook_addr, stub_addr)?, NOP])?;
    Ok(())
}

fn patch_player_stage_lock_vsmode_noop(rom: &mut RomImage) -> Result<()> {
    for (func_addr, stub_addr, original_word) in [
        (0x0212_C130, 0x020C_5390, 0xE92D_4000),
        (0x0212_C1B8, 0x020C_53B0, 0xE92D_4010),
    ] {
        let stub = [
            0xE59F_C010,
            0xE5DC_C000,
            0xE35C_0000,
            0x112F_FF1E,
            original_word,
            encode_b(stub_addr + 0x14, func_addr + 0x04)?,
            0x0208_5A84,
        ];
        ensure_zero_overlay_words(rom, 0, stub_addr, stub.len())?;
        patch_overlay_words_by_id(rom, 0, stub_addr, &stub)?;
        let old = patch_overlay_words(rom, func_addr, &[encode_b(func_addr, stub_addr)?])?;
        let old_word = u32::from_le_bytes(old[..4].try_into()?);
        if old_word != original_word {
            bail!("stage lock hook 0x{func_addr:08x} expected 0x{original_word:08x}, got 0x{old_word:08x}");
        }
    }
    Ok(())
}

fn patch_player_powerup_state_allows_movement_mvl(rom: &mut RomImage) -> Result<()> {
    let branch_addr = 0x020F_D310;
    let skip_normal_update_addr = branch_addr + 4;
    let normal_update_addr = 0x020F_D354;
    let stub_addr = 0x020C_53D0;
    let stub = [
        with_cond(encode_b(stub_addr, normal_update_addr)?, 0),
        encode_ldrb_imm(12, 4, PLAYER_BASE_PREVIOUS_POWERUP_OFFSET)?,
        encode_cmp_imm(12, PLAYER_POWERUP_MEGA)?,
        with_cond(encode_b(stub_addr + 0x0C, normal_update_addr)?, 0),
        encode_ldrb_imm(12, 4, PLAYER_BASE_REQUESTED_POWERUP_OFFSET)?,
        encode_cmp_imm(12, PLAYER_POWERUP_MEGA)?,
        with_cond(encode_b(stub_addr + 0x18, skip_normal_update_addr)?, 0),
        encode_ldrb_imm(12, 4, PLAYER_BASE_CURRENT_POWERUP_OFFSET)?,
        encode_cmp_imm(12, PLAYER_POWERUP_MEGA)?,
        with_cond(encode_b(stub_addr + 0x24, skip_normal_update_addr)?, 0),
        encode_b(stub_addr + 0x28, normal_update_addr)?,
    ];

    ensure_zero_overlay_words(rom, 0, stub_addr, stub.len())?;
    patch_overlay_words_by_id(rom, 0, stub_addr, &stub)?;

    let old =
        patch_overlay_words_by_id(rom, 10, branch_addr, &[encode_b(branch_addr, stub_addr)?])?;
    let old_word = u32::from_le_bytes(old[..4].try_into()?);
    let expected = with_cond(encode_b(branch_addr, normal_update_addr)?, 0);
    if old_word != expected {
        bail!(
            "Player::onUpdate powerup-state branch @ 0x{branch_addr:08x} expected 0x{expected:08x}, got 0x{old_word:08x}"
        );
    }
    Ok(())
}

fn patch_arm9_words(rom: &mut RomImage, addr: u32, words: &[u32]) -> Result<Vec<u8>> {
    let section = rom
        .arm9_sections
        .iter()
        .find(|section| addr >= section.ram_addr && addr < section.ram_addr + section.size as u32)
        .ok_or_else(|| anyhow!("address 0x{addr:08x} is not in an ARM9 data section"))?;
    let off = section.file_off + (addr - section.ram_addr) as usize;
    patch_words(&mut rom.arm9, off, words)
}

fn ensure_zero_arm9_words(rom: &RomImage, addr: u32, count: usize) -> Result<()> {
    let section = rom
        .arm9_sections
        .iter()
        .find(|section| addr >= section.ram_addr && addr < section.ram_addr + section.size as u32)
        .ok_or_else(|| anyhow!("address 0x{addr:08x} is not in an ARM9 data section"))?;
    let off = section.file_off + (addr - section.ram_addr) as usize;
    let len = count
        .checked_mul(4)
        .ok_or_else(|| anyhow!("ARM9 zero check length overflow"))?;
    let bytes = slice(&rom.arm9, off, len, "ARM9 zero cave")?;
    if bytes.iter().any(|byte| *byte != 0) {
        bail!("ARM9 code cave @ 0x{addr:08x} is not empty");
    }
    Ok(())
}

fn patch_arm9_word_checked(
    rom: &mut RomImage,
    addr: u32,
    expected: u32,
    replacement: u32,
    label: &str,
) -> Result<()> {
    let old = patch_arm9_words(rom, addr, &[replacement])?;
    let old_word = u32::from_le_bytes(old[..4].try_into()?);
    if old_word != expected {
        bail!("{label} @ 0x{addr:08x} expected 0x{expected:08x}, got 0x{old_word:08x}");
    }
    Ok(())
}

fn patch_overlay_words(rom: &mut RomImage, addr: u32, words: &[u32]) -> Result<Vec<u8>> {
    let index = overlay_index_for_addr(rom, addr)?;
    let off = (addr - rom.overlays[index].base_addr) as usize;
    patch_words(&mut rom.overlays[index].data, off, words)
}

fn patch_overlay_words_by_id(
    rom: &mut RomImage,
    overlay_id: u32,
    addr: u32,
    words: &[u32],
) -> Result<Vec<u8>> {
    let index = rom
        .overlays
        .iter()
        .position(|overlay| overlay.id == overlay_id)
        .ok_or_else(|| anyhow!("overlay {overlay_id} not found"))?;
    let overlay = &rom.overlays[index];
    if addr < overlay.base_addr || addr >= overlay.base_addr + overlay.data.len() as u32 {
        bail!("address 0x{addr:08x} is not in overlay {overlay_id}");
    }
    let off = (addr - overlay.base_addr) as usize;
    patch_words(&mut rom.overlays[index].data, off, words)
}

fn ensure_zero_overlay_words(
    rom: &RomImage,
    overlay_id: u32,
    addr: u32,
    word_count: usize,
) -> Result<()> {
    let overlay = rom
        .overlays
        .iter()
        .find(|overlay| overlay.id == overlay_id)
        .ok_or_else(|| anyhow!("overlay {overlay_id} not found"))?;
    let off = (addr - overlay.base_addr) as usize;
    let len = word_count * 4;
    if off + len > overlay.data.len() {
        bail!("overlay cave out of range at 0x{addr:08x}");
    }
    if overlay.data[off..off + len].iter().any(|byte| *byte != 0) {
        bail!("overlay cave at 0x{addr:08x} is not empty");
    }
    Ok(())
}

fn overlay_index_for_addr(rom: &RomImage, addr: u32) -> Result<usize> {
    rom.overlays
        .iter()
        .enumerate()
        .filter(|(_, overlay)| {
            addr >= overlay.base_addr && addr < overlay.base_addr + overlay.data.len() as u32
        })
        .min_by_key(|(_, overlay)| overlay.data.len())
        .map(|(index, _)| index)
        .ok_or_else(|| anyhow!("address 0x{addr:08x} is not in an ARM9 overlay"))
}

fn patch_words(data: &mut [u8], off: usize, words: &[u32]) -> Result<Vec<u8>> {
    let len = words.len() * 4;
    if off + len > data.len() {
        bail!("patch out of range: offset=0x{off:x} len=0x{len:x}");
    }
    let old = data[off..off + len].to_vec();
    for (i, word) in words.iter().enumerate() {
        data[off + i * 4..off + i * 4 + 4].copy_from_slice(&word.to_le_bytes());
    }
    Ok(old)
}

fn append_overlay_entry(out: &mut Vec<u8>, overlay: &Overlay, compressed_size: u32) {
    append_u32(out, overlay.id);
    append_u32(out, overlay.base_addr);
    append_u32(out, overlay.code_size);
    append_u32(out, overlay.bss_size);
    append_u32(out, overlay.ctor_start);
    append_u32(out, overlay.ctor_end);
    append_u32(out, overlay.file_id);
    let mut flags = overlay.flags & !0x00ff_ffff;
    if overlay.compressed() {
        flags |= compressed_size & 0x00ff_ffff;
    }
    append_u32(out, flags);
}

impl Overlay {
    fn compressed(&self) -> bool {
        (self.flags & (1 << 24)) != 0
    }
}

fn load_symbols(path: &Path) -> Result<BTreeMap<String, u32>> {
    let text =
        fs::read_to_string(path).with_context(|| format!("read symbols {}", path.display()))?;
    let mut symbols = BTreeMap::new();
    for line in text.lines() {
        let Some((name, rest)) = line.split_once('=') else {
            continue;
        };
        let Some((value, _)) = rest.split_once(';') else {
            continue;
        };
        let name = name.trim();
        if name.is_empty()
            || !name
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'.' || b == b'$')
        {
            continue;
        }
        if let Ok(parsed) = parse_u32(value.trim()) {
            symbols.insert(name.to_owned(), parsed);
        }
    }
    Ok(symbols)
}

fn symbol(symbols: &BTreeMap<String, u32>, name: &str) -> Result<u32> {
    symbols
        .get(name)
        .copied()
        .ok_or_else(|| anyhow!("symbol not found: {name}"))
}

fn parse_u32(value: &str) -> Result<u32> {
    let trimmed = value.trim();
    if let Some(hex) = trimmed
        .strip_prefix("0x")
        .or_else(|| trimmed.strip_prefix("0X"))
    {
        Ok(u32::from_str_radix(hex, 16)?)
    } else {
        Ok(u32::from_str_radix(trimmed, 16).or_else(|_| trimmed.parse())?)
    }
}

mod binary;
#[cfg(test)]
mod tests;

use binary::*;
