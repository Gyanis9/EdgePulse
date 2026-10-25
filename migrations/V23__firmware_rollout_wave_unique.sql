-- 波次下标在发布内唯一：协调器物化波次时靠这条唯一索引兜底。
CREATE UNIQUE INDEX uq_fw_rollout_wave_index ON biz_firmware_rollout_wave (rollout_id, wave_index);
