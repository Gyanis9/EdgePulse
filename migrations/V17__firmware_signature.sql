-- 固件真实性：签名字节（Base64）、信任键标识与签名算法。
ALTER TABLE biz_firmware ADD COLUMN signature VARCHAR(256) NOT NULL DEFAULT '';
ALTER TABLE biz_firmware ADD COLUMN signing_key_id VARCHAR(64) NOT NULL DEFAULT '';
ALTER TABLE biz_firmware ADD COLUMN signature_algorithm VARCHAR(32) NOT NULL DEFAULT '';
