use super::{
    big_star_selector, build_direct_loadlevel_stub, build_eight_coin_reward_positional_sfx_stub,
    build_game_tick_input_gate, build_is_out_of_view_vertical_camera_fallback_stub,
    encode_add_reg_lsl, encode_cmp_imm, encode_ldr_imm, encode_load_imm, encode_mov_imm,
    encode_str_imm, encode_strb_imm, initial_lives, life_mode_selector, stage_scene_settings,
    with_cond, DirectMvlConfig, EIGHT_COIN_SFX_ID, GAME_PLAYER_INVENTORY_POWERUP_ADDR,
    GAME_TICK_PROBE_HISTORY_ADDR, GAME_TICK_PROBE_HISTORY_CAPACITY,
    GAME_TICK_PROBE_HISTORY_ENTRY_BYTES, GAME_TICK_PROBE_MAGIC, MVL_NATIVE_COURSE_SELECTOR_ADDR,
    MVL_RUNTIME_CONFIG_STAGE_OFFSET, PLAYER_POWERUP_MEGA,
};

#[test]
fn stage_scene_settings_follow_mvl_course_ids() {
    assert_eq!(stage_scene_settings(0).unwrap(), 0x00b4_ff00);
    assert_eq!(stage_scene_settings(4).unwrap(), 0x00b8_ff00);
    assert!(stage_scene_settings(5).is_err());
}

#[test]
fn initial_lives_keep_rules_separate_from_stage_settings() {
    assert_eq!(initial_lives("3").unwrap(), 3);
    assert_eq!(initial_lives("5").unwrap(), 5);
    assert_eq!(initial_lives("endless").unwrap(), 3);
    assert_eq!(life_mode_selector("3").unwrap(), 0);
    assert_eq!(life_mode_selector("5").unwrap(), 0);
    assert_eq!(life_mode_selector("endless").unwrap(), 2);
}

#[test]
fn big_star_targets_use_the_native_selector_table() {
    assert_eq!(big_star_selector(3).unwrap(), 0);
    assert_eq!(big_star_selector(5).unwrap(), 1);
    assert_eq!(big_star_selector(10).unwrap(), 2);
}

#[test]
fn mega_powerup_exception_uses_the_native_mega_enum() {
    assert_eq!(
        PLAYER_POWERUP_MEGA, 3,
        "PowerupState 3 is Mega; 4 is Mini and 5 is Shell"
    );
}

#[test]
fn direct_loadlevel_uses_network_random_seed() {
    let config = DirectMvlConfig {
        stage: 2,
        player_id: 0,
        scene_settings: 0x00b6_ff00,
        initial_lives: 3,
        life_mode_selector: 0,
        big_star_selector: 1,
    };
    let stub = build_direct_loadlevel_stub(0x0215_0000, 0x0200_0000, 0x0210_0000, &config)
        .expect("build direct MvL stub");
    let load_network_rng_seed = encode_load_imm(12, 0xffff_ffff).expect("encode rng seed");
    let store_rng_seed = encode_str_imm(12, 13, 0x30).expect("encode rng seed store");

    assert!(
        stub.windows(2)
            .any(|pair| pair == [load_network_rng_seed, store_rng_seed]),
        "loadLevel rngSeed stack argument must be 0xffffffff so match-seeded Net/Game RNG is used"
    );
}

#[test]
fn direct_loadlevel_matches_normal_mvl_control_args() {
    let config = DirectMvlConfig {
        stage: 2,
        player_id: 0,
        scene_settings: 0x00b6_ff00,
        initial_lives: 3,
        life_mode_selector: 0,
        big_star_selector: 1,
    };
    let stub = build_direct_loadlevel_stub(0x0215_0000, 0x0200_0000, 0x0210_0000, &config)
        .expect("build direct MvL stub");

    let store_control_flag = encode_str_imm(12, 13, 0x20).expect("encode control flag store");
    let load_control_options = encode_load_imm(12, 0xff).expect("encode control options");
    let store_control_options = encode_str_imm(12, 13, 0x24).expect("encode control options store");

    assert!(
        stub.contains(&store_control_flag),
        "loadLevel stack arg 0x20 must match the normal MvL load path"
    );
    assert!(
        stub.windows(2)
            .any(|pair| pair == [load_control_options, store_control_options]),
        "loadLevel stack arg 0x24 must match the normal MvL load path"
    );
}

#[test]
fn direct_loadlevel_updates_native_course_selector() {
    let config = DirectMvlConfig {
        stage: 4,
        player_id: 0,
        scene_settings: 0x00b8_ff00,
        initial_lives: 3,
        life_mode_selector: 2,
        big_star_selector: 1,
    };
    let stub = build_direct_loadlevel_stub(0x0215_0000, 0x0200_0000, 0x0210_0000, &config)
        .expect("build direct MvL stub");

    let load_fallback_stage = encode_mov_imm(1, config.stage as u32).expect("encode stage load");
    let load_runtime_stage = with_cond(
        encode_ldr_imm(1, 4, MVL_RUNTIME_CONFIG_STAGE_OFFSET).expect("encode runtime stage load"),
        0,
    );
    let store_course_selector = encode_strb_imm(1, 0, 0).expect("encode course selector store");

    assert!(
        stub.contains(&MVL_NATIVE_COURSE_SELECTOR_ADDR),
        "stub must reference the native MvL course selector used by 8-coin item filtering"
    );
    assert!(
        stub.windows(3).any(|window| window
            == [
                load_fallback_stage,
                load_runtime_stage,
                store_course_selector
            ]),
        "stub must store fallback/runtime stage into the native MvL course selector"
    );
}

#[test]
fn direct_loadlevel_clears_initial_inventory_powerups() {
    let config = DirectMvlConfig {
        stage: 0,
        player_id: 1,
        scene_settings: 0x00b4_ff00,
        initial_lives: 3,
        life_mode_selector: 0,
        big_star_selector: 1,
    };
    let stub = build_direct_loadlevel_stub(0x0215_0000, 0x0200_0000, 0x0210_0000, &config)
        .expect("build direct MvL stub");

    let clear_value = encode_mov_imm(1, 0).expect("encode inventory clear value");
    let clear_player0 = encode_strb_imm(1, 0, 0).expect("encode player0 inventory clear");
    let clear_player1 = encode_strb_imm(1, 0, 1).expect("encode player1 inventory clear");

    assert!(
        stub.contains(&GAME_PLAYER_INVENTORY_POWERUP_ADDR),
        "stub must reference Game::playerInventoryPowerup"
    );
    assert!(
        stub.windows(3)
            .any(|window| window == [clear_value, clear_player0, clear_player1]),
        "stub must clear Mario and Luigi initial stock items after direct MvL load"
    );
}

#[test]
fn eight_coin_reward_sfx_uses_reward_player_world_position() {
    let start_addr = 0x020c_5300;
    let get_player_addr = 0x0202_0608;
    let play_sfx_addr = 0x0201_2398;
    let stub =
        build_eight_coin_reward_positional_sfx_stub(start_addr, get_player_addr, play_sfx_addr)
            .expect("build positional 8-coin SFX stub");

    assert_eq!(stub.len(), 8);
    assert_eq!(stub[1], 0xE1A0_0005, "reward player ID must come from r5");
    assert_eq!(
        stub[3], 0xE280_105C,
        "SFX position must be player actor world Vec3 at +0x5c"
    );
    assert_eq!(stub[7], EIGHT_COIN_SFX_ID);
}

#[test]
fn vertical_out_of_view_fallback_preserves_player1_camera_slot() {
    let stub = build_is_out_of_view_vertical_camera_fallback_stub(0x020c_5298)
        .expect("build vertical out-of-view fallback stub");

    let compare_player1 = encode_cmp_imm(2, 1).expect("encode player1 compare");
    let force_slot0 = with_cond(encode_mov_imm(2, 0).expect("encode slot0 move"), 0);
    let compare_height_zero = encode_cmp_imm(12, 0).expect("encode camera-height compare");

    assert!(
        !stub
            .windows(2)
            .any(|window| window == [compare_player1, force_slot0]),
        "player1 must keep camera slot 1; forcing it to slot0 causes Luigi pit deaths"
    );
    assert!(
        stub.windows(2)
            .any(|window| window == [compare_height_zero, force_slot0]),
        "fallback should still use slot0 only when the requested camera height is zero"
    );
}

#[test]
fn game_tick_history_preserves_two_player_touch_metadata() {
    const INPUT_GATE_ADDR: u32 = 0x0200_1b40;
    const PROCESS_STAGE_GATE_ADDR: u32 = 0x0200_1d00;
    const INPUT_UPDATE_ADDR: u32 = 0x0200_5230;
    let gate = build_game_tick_input_gate(INPUT_GATE_ADDR, INPUT_UPDATE_ADDR)
        .expect("build game-tick input gate");

    assert_eq!(GAME_TICK_PROBE_MAGIC, 0x3250_5447);
    assert_eq!(GAME_TICK_PROBE_HISTORY_ADDR, 0x023c_1300);
    assert_eq!(GAME_TICK_PROBE_HISTORY_CAPACITY, 12);
    assert_eq!(GAME_TICK_PROBE_HISTORY_ENTRY_BYTES, 16);
    assert!(
        INPUT_GATE_ADDR + gate.len() as u32 * 4 <= PROCESS_STAGE_GATE_ADDR,
        "touch-capable input gate must fit before the process-stage gate"
    );
    assert!(
        gate.contains(&GAME_TICK_PROBE_HISTORY_ADDR),
        "gate must load replay entries from the dedicated scratch history"
    );
    assert!(
        gate.contains(&encode_add_reg_lsl(2, 2, 12, 4).expect("encode 16-byte stride")),
        "gate must index 16-byte history entries"
    );
    assert!(
        gate.windows(2).any(|window| {
            window
                == [
                    encode_ldr_imm(3, 2, 8).expect("encode player0 metadata load"),
                    encode_ldr_imm(2, 2, 12).expect("encode player1 metadata load"),
                ]
        }),
        "gate must load both players' action/touch/x/y words"
    );
    assert!(
        gate.contains(&encode_str_imm(3, 14, 0x44).expect("encode player0 metadata store"))
            && gate.contains(&encode_str_imm(2, 14, 0x84).expect("encode player1 metadata store"),),
        "gate must restore both metadata words into JIT scratch packets"
    );
}
