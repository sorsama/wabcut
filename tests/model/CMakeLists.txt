wabcut_add_test_suite(model
    SOURCES
        TimelineFixture.cpp
        TimelineFixture.h
        ScaleProject.cpp
        ScaleProject.h
        essay_templates_author.cpp
        essay_templates_author.h
        gaming_templates_author.cpp
        gaming_templates_author.h
        test_beat_grid.cpp
        test_builtin_templates.cpp
        test_bulk_build.cpp
        test_caption_workshop.cpp
        test_project_downgrade.cpp # 0.7 (card 059): Save for an Older Version
        test_captions.cpp
        test_changeset.cpp
        test_clipboard.cpp
        test_color_management.cpp
        test_cliplist.cpp
        test_edit_move.cpp
        test_timeline_parity.cpp
        test_edit_remove.cpp
        test_edit_session.cpp
        test_edit_source.cpp
        test_edit_trim.cpp
        test_editing_ops.cpp
        test_effect_data.cpp
        test_effect_presets.cpp
        test_masks.cpp
        test_effects.cpp
        test_fixture.cpp
        test_field_options.cpp
        test_frame_scaling.cpp
        test_vr.cpp # 0.7 (card 38): VR / 360 video
        test_gaming_ops.cpp
        test_graphic_ops.cpp
        test_graphics.cpp
        test_graphics_roll.cpp
        test_graphics_styles.cpp
        test_graphics_text.cpp
        test_graphics_typography.cpp # 0.7 (card #138): OpenType features and Optical kerning
        test_history.cpp
        test_keyframes.cpp
        test_layouts.cpp
        test_library_ops.cpp
        test_project_panel.cpp
        test_locked_links.cpp
        test_marks_snap.cpp
        test_master_clip_effects.cpp
        test_auditions.cpp
        test_media_locator.cpp
        test_media_paths.cpp
        test_merge.cpp
        test_motion_model.cpp
        test_nesting.cpp
        test_parameters.cpp
        test_parity.cpp
        test_project_diff.cpp
        test_perf.cpp
        test_project_io.cpp
        test_project_ops.cpp
        test_project_state.cpp
        test_redaction_media.cpp
        test_r08_c3.cpp
        test_clip_lock.cpp # 0.7 (batch A6): Lock Clip and Move Track
        test_r08_c5.cpp
        test_sequence_extent.cpp
        test_sequence_settings.cpp
        test_session_history.cpp
        test_snapshot_releaser.cpp
        test_source_marks.cpp
        test_source_tracks.cpp
        test_markers.cpp
        test_speed.cpp
        test_spelling.cpp
        test_text_path.cpp
        test_time.cpp
        test_time_checked.cpp
        test_remap_review.cpp
        test_review_notes.cpp
        test_scale.cpp
        test_time_remap.cpp
        test_timecode.cpp
        test_titles.cpp
        test_track_color.cpp
        test_track_effects.cpp
        test_track_faders.cpp
        test_track_groups.cpp # 0.7 (batch A9, card 124): track groups
        test_video_solo.cpp # 0.7 (card 106): video track solo
        test_audio_takes.cpp
        test_voice_over_model.cpp
        test_audio_channels_model.cpp
        test_transition_ops.cpp
        test_transitions.cpp
    LIBS wabcut_model)

# P.2 (E6, monitors): Motion set as one edit (MotionOps.h).
target_sources(wabcut_model_tests PRIVATE test_motion_ops.cpp)
# 0.7: the AI video effects' descriptors, chain and hash (AiEffects.h).
target_sources(wabcut_model_tests PRIVATE test_ai_effects.cpp)
target_sources(wabcut_model_tests PRIVATE test_eye_contact.cpp)

# 0.7 (card 029): where the .bakN copies and autosave files of a project are kept (beside it, an Auto-Save folder, a chosen root).
target_sources(wabcut_model_tests PRIVATE test_backup_location.cpp)
# 0.7 (card 046): a split edit (L/J cut) stays split through trims, ripples and moves.
target_sources(wabcut_model_tests PRIVATE test_split_edit_ops.cpp)
# 0.7 (batch A7): several markers at once (card 119), keyframes that follow edits (card 120), notes on clips (card 125).
target_sources(wabcut_model_tests PRIVATE test_a7_model.cpp)
# 0.7 (card 180): the pacing heat strip's analysis.
target_sources(wabcut_model_tests PRIVATE test_a7_pacing.cpp)
# 0.7 (card 069): Render and Replace for nested sequences, Restore Unrendered.
target_sources(wabcut_model_tests PRIVATE test_render_replace.cpp)
# 0.7 (batch A10): the Graph Editor's easing presets (card 96) and Motion Blur's shutter instants and descriptor (card 58).
target_sources(wabcut_model_tests PRIVATE test_a10_model.cpp)
