<script setup lang="ts">
import { computed, nextTick, ref, watch } from 'vue'
import Modal from '@/components/ui/Modal.vue'
import Button from '@/components/ui/Button.vue'
import Field from '@/components/ui/Field.vue'
import TextInput from '@/components/ui/TextInput.vue'
import Tag from '@/components/ui/Tag.vue'
import { ApiError, authApi, type MfaSetupResponse, type MfaStatusResponse } from '@/api'
import { pushToast, toastError } from '@/composables/toast'
import { copyText } from '@/utils/clipboard'
import { required, validate } from '@/utils/validate'

const props = defineProps<{ open: boolean }>()
const emit = defineEmits<{ (e: 'close'): void }>()

/* 弹窗内按阶段切换内容；密钥与恢复码只活在本次打开期间 */
type Stage = 'overview' | 'setup' | 'codes' | 'disable' | 'regenerate'

const stage = ref<Stage>('overview')
const status = ref<MfaStatusResponse | null>(null)
const loading = ref(false)
const saving = ref(false)
const statusError = ref('')
const formError = ref('')
const password = ref('')
const code = ref('')
const setup = ref<MfaSetupResponse | null>(null)
const recoveryCodes = ref<string[]>([])
const qrCanvas = ref<HTMLCanvasElement | null>(null)
const qrError = ref('')
const errors = ref<{ password?: string; code?: string }>({})

const enabled = computed(() => Boolean(status.value?.enabled))
const recoveryCount = computed(() => status.value?.recoveryCodesRemaining ?? 0)

/* 每次打开或发起动作都递增；关闭后到达的响应凭它识别并丢弃，避免把密钥或恢复码带回界面 */
let operation = 0

function nextOperation() {
  return ++operation
}

function isCurrent(token: number) {
  return props.open && token === operation
}

const totpCodeRule = (value: unknown) => {
  const text = String(value ?? '').trim()
  if (!text) return '请输入 6 位动态码'
  return /^\d{6}$/.test(text) ? undefined : '动态码应为 6 位数字'
}

function messageOf(error: unknown, fallback: string) {
  return error instanceof ApiError ? error.message : fallback
}

function reset() {
  operation += 1
  stage.value = 'overview'
  status.value = null
  loading.value = false
  saving.value = false
  statusError.value = ''
  formError.value = ''
  password.value = ''
  code.value = ''
  setup.value = null
  recoveryCodes.value = []
  qrError.value = ''
  errors.value = {}
}

function backToOverview() {
  stage.value = 'overview'
  formError.value = ''
  password.value = ''
  code.value = ''
  setup.value = null
  recoveryCodes.value = []
  qrError.value = ''
  errors.value = {}
}

async function loadStatus() {
  const token = nextOperation()
  loading.value = true
  statusError.value = ''
  try {
    const data = await authApi.mfaStatus()
    if (!isCurrent(token)) return
    status.value = data
  } catch (error) {
    if (!isCurrent(token)) return
    statusError.value = messageOf(error, '两步验证状态加载失败')
  } finally {
    if (isCurrent(token)) loading.value = false
  }
}

watch(() => props.open, (open) => {
  reset()
  if (open) void loadStatus()
}, { immediate: true })

async function renderQr() {
  const snapshot = setup.value
  const token = operation
  await nextTick()
  if (!isCurrent(token) || !snapshot || setup.value !== snapshot || !qrCanvas.value) return
  try {
    // 按需加载编码器，登录页与首屏不必带上二维码库
    const { default: QRCode } = await import('qrcode')
    if (!isCurrent(token) || setup.value !== snapshot || !qrCanvas.value) return
    await QRCode.toCanvas(qrCanvas.value, snapshot.otpauthUri, { width: 180, margin: 1 })
    if (!isCurrent(token) || setup.value !== snapshot) return
    qrError.value = ''
  } catch {
    if (isCurrent(token) && setup.value === snapshot) {
      qrError.value = '二维码生成失败，请改用下方密钥手动添加'
    }
  }
}

watch([stage, setup], () => {
  if (props.open && stage.value === 'setup' && setup.value) void renderQr()
})

function startSetup() {
  stage.value = 'setup'
  formError.value = ''
  password.value = ''
  code.value = ''
  setup.value = null
  errors.value = {}
}

function openRegenerate() {
  stage.value = 'regenerate'
  formError.value = ''
  code.value = ''
  errors.value = {}
}

function openDisable() {
  stage.value = 'disable'
  formError.value = ''
  password.value = ''
  code.value = ''
  errors.value = {}
}

async function beginSetup() {
  if (saving.value) return
  formError.value = ''
  errors.value = validate({ password: password.value }, { password: [required('请输入当前密码')] })
  if (Object.keys(errors.value).length) return
  const token = nextOperation()
  saving.value = true
  try {
    const result = await authApi.setupMfa({ password: password.value })
    if (!isCurrent(token)) return
    setup.value = result
    code.value = ''
    errors.value = {}
  } catch (error) {
    if (!isCurrent(token)) return
    formError.value = messageOf(error, '无法开始设置两步验证')
    toastError(error, '无法开始设置两步验证')
  } finally {
    if (isCurrent(token)) saving.value = false
  }
}

async function enable() {
  if (saving.value) return
  formError.value = ''
  errors.value = validate(
    { password: password.value, code: code.value },
    { password: [required('请输入当前密码')], code: [totpCodeRule] },
  )
  if (Object.keys(errors.value).length) return
  const token = nextOperation()
  saving.value = true
  try {
    const result = await authApi.enableMfa({ password: password.value, code: code.value.trim() })
    if (!isCurrent(token)) return
    recoveryCodes.value = result.recoveryCodes
    status.value = { enabled: true, recoveryCodesRemaining: result.recoveryCodes.length }
    setup.value = null
    password.value = ''
    code.value = ''
    stage.value = 'codes'
    pushToast('两步验证已启用')
  } catch (error) {
    if (!isCurrent(token)) return
    formError.value = messageOf(error, '两步验证启用失败')
    toastError(error, '两步验证启用失败')
  } finally {
    if (isCurrent(token)) saving.value = false
  }
}

async function disable() {
  if (saving.value) return
  formError.value = ''
  errors.value = validate(
    { password: password.value, code: code.value },
    { password: [required('请输入当前密码')], code: [totpCodeRule] },
  )
  if (Object.keys(errors.value).length) return
  const token = nextOperation()
  saving.value = true
  try {
    await authApi.disableMfa({ password: password.value, code: code.value.trim() })
    if (!isCurrent(token)) return
    status.value = { enabled: false, recoveryCodesRemaining: 0 }
    password.value = ''
    code.value = ''
    stage.value = 'overview'
    pushToast('两步验证已关闭')
  } catch (error) {
    if (!isCurrent(token)) return
    formError.value = messageOf(error, '两步验证关闭失败')
    toastError(error, '两步验证关闭失败')
  } finally {
    if (isCurrent(token)) saving.value = false
  }
}

async function regenerate() {
  if (saving.value) return
  formError.value = ''
  errors.value = validate({ code: code.value }, { code: [totpCodeRule] })
  if (Object.keys(errors.value).length) return
  const token = nextOperation()
  saving.value = true
  try {
    const result = await authApi.recoveryCodes({ code: code.value.trim() })
    if (!isCurrent(token)) return
    recoveryCodes.value = result.recoveryCodes
    status.value = { enabled: true, recoveryCodesRemaining: result.recoveryCodes.length }
    code.value = ''
    stage.value = 'codes'
    pushToast('恢复码已重新生成')
  } catch (error) {
    if (!isCurrent(token)) return
    formError.value = messageOf(error, '恢复码生成失败')
    toastError(error, '恢复码生成失败')
  } finally {
    if (isCurrent(token)) saving.value = false
  }
}

function finishCodes() {
  recoveryCodes.value = []
  code.value = ''
  stage.value = 'overview'
}

/* 恢复码只显示一次，保存确认前不接受标题栏、Escape 与返回操作 */
function onDialogClose() {
  if (stage.value === 'codes') return
  emit('close')
}

function onCancel() {
  if (saving.value) return
  if (stage.value === 'overview') emit('close')
  else if (stage.value !== 'codes') backToOverview()
}

async function copySecret() {
  if (!setup.value) return
  const copied = await copyText(setup.value.secret)
  pushToast(copied ? '密钥已复制' : '复制失败，请手动选择密钥文本', copied ? 'ok' : 'danger')
}

async function copyUri() {
  if (!setup.value) return
  const copied = await copyText(setup.value.otpauthUri)
  pushToast(copied ? '链接已复制' : '复制失败，请手动选择链接文本', copied ? 'ok' : 'danger')
}

async function copyCodes() {
  const copied = await copyText(recoveryCodes.value.join('\n'))
  pushToast(copied ? '恢复码已复制' : '复制失败，请手动选择恢复码文本', copied ? 'ok' : 'danger')
}
</script>

<template>
  <Modal :open="open" title="两步验证" sub="使用身份验证器（TOTP）为账号增加第二道验证。" :width="640" :closable="stage !== 'codes'" @close="onDialogClose">
    <div class="mfa-panel">
      <div v-if="loading" class="mfa-loading"><span class="spinner" aria-hidden="true" />正在读取两步验证状态…</div>
      <div v-else-if="statusError" class="mfa-status-error">
        <p class="login-error" role="alert">{{ statusError }}</p>
        <Button variant="secondary" size="sm" icon="refresh" @click="loadStatus">重试</Button>
      </div>
      <template v-else>
        <template v-if="stage === 'overview'">
          <div class="mfa-status">
            <div class="mfa-status-head">
              <Tag :tone="enabled ? 'ok' : 'neutral'" dot>{{ enabled ? '已启用' : '未启用' }}</Tag>
              <span v-if="enabled" class="soft">剩余 {{ recoveryCount }} 个恢复码</span>
            </div>
            <p class="muted">
              {{ enabled
                ? '登录时除密码外，还需输入身份验证器生成的 6 位动态码。恢复码在丢失验证器时使用，每枚只能使用一次。'
                : '启用后，登录时需要同时输入密码和身份验证器动态码；请先安装任意兼容 TOTP 的身份验证器应用。' }}
            </p>
            <ol v-if="!enabled" class="mfa-steps">
              <li>确认当前密码，生成专属密钥</li>
              <li>用身份验证器扫描二维码或手动输入密钥</li>
              <li>输入动态码完成启用，并保存一次性恢复码</li>
            </ol>
            <p v-if="enabled && recoveryCount === 0" class="field-hint">恢复码已用完，建议立即重新生成。</p>
          </div>
        </template>

        <template v-else-if="stage === 'setup'">
          <template v-if="!setup">
            <p class="muted">开始设置前需要确认当前密码。密钥只在本次设置期间显示，关闭对话框即丢弃。</p>
            <Field label="当前密码" required :error="errors.password" span>
              <TextInput v-model="password" type="password" autocomplete="current-password" :invalid="!!errors.password" :disabled="saving" @enter="beginSetup" />
            </Field>
          </template>
          <template v-else>
            <p class="muted">用身份验证器扫描二维码，或手动输入下方密钥；然后输入验证器生成的 6 位动态码完成启用。</p>
            <div class="mfa-setup">
              <div class="mfa-qr">
                <canvas ref="qrCanvas" aria-label="两步验证二维码" />
                <p v-if="qrError" class="field-hint">{{ qrError }}</p>
              </div>
              <div class="mfa-secret-block">
                <div class="section-label">密钥</div>
                <pre class="code-block mfa-secret">{{ setup.secret }}</pre>
                <Button variant="text" size="sm" icon="copy" @click="copySecret">复制密钥</Button>
              </div>
            </div>
            <div class="mfa-uri-block">
              <div class="section-label">otpauth 链接</div>
              <pre class="code-block mfa-uri">{{ setup.otpauthUri }}</pre>
              <Button variant="text" size="sm" icon="copy" @click="copyUri">复制链接</Button>
            </div>
            <Field label="当前密码" required :error="errors.password" span>
              <TextInput v-model="password" type="password" autocomplete="current-password" :invalid="!!errors.password" :disabled="saving" />
            </Field>
            <Field label="动态验证码" required :error="errors.code" hint="身份验证器每 30 秒刷新一次" span>
              <TextInput v-model="code" placeholder="6 位数字" :maxlength="6" autocomplete="one-time-code" :invalid="!!errors.code" :disabled="saving" @enter="enable" />
            </Field>
          </template>
        </template>

        <template v-else-if="stage === 'codes'">
          <div class="token-once">
            <div>
              <div class="section-label">恢复码只显示这一次</div>
              <p class="faint" style="margin: 4px 0 0">请立即保存。关闭对话框后无法再次查看；每枚恢复码只能使用一次。</p>
            </div>
            <div class="code-block mfa-codes">
              <code v-for="item in recoveryCodes" :key="item">{{ item }}</code>
            </div>
            <div class="token-actions">
              <Button variant="primary" size="sm" icon="copy" @click="copyCodes">复制全部</Button>
              <Tag tone="danger" dot>共 {{ recoveryCodes.length }} 枚</Tag>
            </div>
          </div>
        </template>

        <template v-else-if="stage === 'disable'">
          <p class="muted">关闭后登录只需密码。请输入当前密码和身份验证器动态码确认。</p>
          <Field label="当前密码" required :error="errors.password" span>
            <TextInput v-model="password" type="password" autocomplete="current-password" :invalid="!!errors.password" :disabled="saving" @enter="disable" />
          </Field>
          <Field label="动态验证码" required :error="errors.code" span>
            <TextInput v-model="code" placeholder="6 位数字" :maxlength="6" autocomplete="one-time-code" :invalid="!!errors.code" :disabled="saving" @enter="disable" />
          </Field>
        </template>

        <template v-else>
          <p class="muted">重新生成会使现有恢复码全部失效。请输入身份验证器动态码确认。</p>
          <Field label="动态验证码" required :error="errors.code" span>
            <TextInput v-model="code" placeholder="6 位数字" :maxlength="6" autocomplete="one-time-code" :invalid="!!errors.code" :disabled="saving" @enter="regenerate" />
          </Field>
        </template>

        <p v-if="formError" class="form-error" role="alert">{{ formError }}</p>
      </template>
    </div>

    <template #footer>
      <Button v-if="stage !== 'codes'" variant="ghost" :disabled="saving" @click="onCancel">{{ stage === 'overview' ? '关闭' : '返回' }}</Button>
      <template v-if="!loading && !statusError">
        <Button v-if="stage === 'overview' && !enabled" variant="primary" icon="shield" @click="startSetup">开始设置</Button>
        <Button v-if="stage === 'overview' && enabled" variant="secondary" icon="refresh" @click="openRegenerate">重新生成恢复码</Button>
        <Button v-if="stage === 'overview' && enabled" variant="primary" danger @click="openDisable">关闭两步验证</Button>
        <Button v-if="stage === 'setup' && !setup" variant="primary" :loading="saving" @click="beginSetup">生成密钥</Button>
        <Button v-if="stage === 'setup' && setup" variant="primary" :loading="saving" @click="enable">启用两步验证</Button>
        <Button v-if="stage === 'codes'" variant="primary" @click="finishCodes">我已保存</Button>
        <Button v-if="stage === 'disable'" variant="primary" danger :loading="saving" @click="disable">确认关闭</Button>
        <Button v-if="stage === 'regenerate'" variant="primary" :loading="saving" @click="regenerate">生成新恢复码</Button>
      </template>
    </template>
  </Modal>
</template>

<style scoped>
.mfa-panel { display: flex; flex-direction: column; gap: 16px; min-height: 120px; }
.mfa-loading { display: flex; align-items: center; gap: 10px; color: var(--ink-soft); font-size: 13px; }
.mfa-status { display: flex; flex-direction: column; gap: 10px; }
.mfa-status-head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.mfa-panel .muted { margin: 0; font-size: 13px; line-height: 1.75; }
.mfa-steps { margin: 0; padding-left: 18px; font-size: 13px; line-height: 1.8; color: var(--ink-muted); }
.mfa-setup { display: flex; gap: 18px; align-items: flex-start; flex-wrap: wrap; }
.mfa-qr {
  flex: none;
  padding: 10px;
  border-radius: var(--r-md);
  background: #fff;
  box-shadow: inset 0 0 0 1px var(--line);
  line-height: 0;
}
.mfa-qr canvas { display: block; width: 180px; height: 180px; }
.mfa-secret-block { display: flex; flex-direction: column; align-items: flex-start; gap: 6px; flex: 1; min-width: 200px; }
.mfa-secret { margin: 0; white-space: pre-wrap; word-break: break-all; letter-spacing: .06em; }
.mfa-uri-block { display: flex; flex-direction: column; align-items: flex-start; gap: 6px; }
.mfa-uri { margin: 0; white-space: pre-wrap; word-break: break-all; font-size: 12px; }
.mfa-codes {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 7px 24px;
  font-size: 13px;
  letter-spacing: .06em;
}
.mfa-status-error { display: flex; flex-direction: column; align-items: flex-start; gap: 10px; }

@media (max-width: 600px) {
  .mfa-codes { grid-template-columns: 1fr; }
  .mfa-qr { width: 100%; display: flex; flex-direction: column; align-items: center; }
}
</style>
