<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import Icon from '@/components/Icon.vue'
import Button from '@/components/ui/Button.vue'
import Field from '@/components/ui/Field.vue'
import TextInput from '@/components/ui/TextInput.vue'
import Checkbox from '@/components/ui/Checkbox.vue'
import { ApiError, authApi, portalApi, type MfaChallenge } from '@/api'
import NeuralNet from '@/components/NeuralNet.vue'
import { completeMfaLogin, login, loginWithOidc } from '@/composables/auth'
import { EASE, afterTwoFrames, gsap, motionSkipped, prefersReducedMotion, trackGlow } from '@/composables/motion'
import { required, validate } from '@/utils/validate'
import { APP } from '@/data'

const router = useRouter()
const route = useRoute()

const form = reactive({ tenantCode: 'default', username: '', password: '', captcha: '' })
const errors = ref<Partial<Record<keyof typeof form, string>>>({})
const captchaImage = computed(() => (captchaSvg.value ? `data:image/svg+xml;charset=utf-8,${encodeURIComponent(captchaSvg.value)}` : ''))
const remember = ref(true)
const submitting = ref(false)
const formError = ref('')
const captchaCode = ref('')
const captchaSvg = ref('')
const captchaUuid = ref('')
const captchaLoading = ref(false)
const root = ref<HTMLElement | null>(null)
const card = ref<HTMLElement | null>(null)
const succeeded = ref(false)
const oidcEnabled = ref(false)

/* ── 第二因素：密码或企业登录确认后只拿挑战，完成动态码校验才落会话 ── */
const mfaStep = ref(false)
const mfaChallenge = ref<MfaChallenge | null>(null)
const mfaCode = ref('')
const mfaRecovery = ref(false)
const mfaError = ref('')
const mfaFieldError = ref('')
const mfaTarget = ref('/')
const mfaRemember = ref(true)

/* ── 左侧仪表：实时时钟（秒针刻度逐格推进）与服务健康 ── */
const now = ref(new Date())
const health = ref<{ ok: boolean; latency: number | null }>({ ok: false, latency: null })
const clock = computed(() => now.value.toTimeString().slice(0, 8))
const dateLine = computed(() => {
  const d = now.value
  return `${d.getFullYear()}.${String(d.getMonth() + 1).padStart(2, '0')}.${String(d.getDate()).padStart(2, '0')} · ${'SUN MON TUE WED THU FRI SAT'.split(' ')[d.getDay()]}`
})
const TICKS = Array.from({ length: 60 }, (_, i) => {
  const a = (i / 60) * Math.PI * 2 - Math.PI / 2
  const major = i % 5 === 0
  return { i, x1: 50 + Math.cos(a) * (major ? 40 : 43), y1: 50 + Math.sin(a) * (major ? 40 : 43), x2: 50 + Math.cos(a) * 47, y2: 50 + Math.sin(a) * 47, major }
})
let clockTimer = 0

async function probeHealth() {
  const start = performance.now()
  try {
    const res = await portalApi.health()
    health.value = { ok: res.status === 'UP', latency: Math.round(performance.now() - start) }
  } catch {
    health.value = { ok: false, latency: null }
  }
}

/* ── 登录卡随指针轻倾（≤ 4°），离开回正；只在可悬停设备上启用 ── */
let tiltX: ((v: number) => void) | null = null
let tiltY: ((v: number) => void) | null = null
function onTilt(event: PointerEvent) {
  if (!card.value || !tiltX || !tiltY || motionSkipped()) return
  const r = card.value.getBoundingClientRect()
  const px = (event.clientX - r.left) / r.width - 0.5
  const py = (event.clientY - r.top) / r.height - 0.5
  tiltY(Math.max(-1, Math.min(1, px * 2)) * 4)
  tiltX(Math.max(-1, Math.min(1, py * 2)) * -3)
}
function resetTilt() {
  tiltX?.(0)
  tiltY?.(0)
}

/* ── 登录成功三拍：按钮收拢成圆（放）→ 对勾圆环描出（结）→ 进入外壳 ── */
const netBurst = ref(0)
async function playSuccess() {
  succeeded.value = true
  netBurst.value += 1
  await nextTick()
  const orb = card.value?.querySelector<HTMLElement>('.success-orb')
  if (!orb || motionSkipped()) return
  const ring = orb.querySelector<SVGCircleElement>('circle')
  const check = orb.querySelector<SVGPathElement>('path')
  await new Promise<void>((resolve) => {
    const tl = gsap.timeline({ onComplete: resolve })
    tl.fromTo(orb, { width: orb.parentElement!.clientWidth }, { width: 46, duration: 0.42, ease: EASE.swap })
    if (ring) tl.fromTo(ring, { strokeDashoffset: 126 }, { strokeDashoffset: 0, duration: 0.5, ease: EASE.reveal }, '-=0.08')
    if (check) tl.fromTo(check, { strokeDashoffset: 24 }, { strokeDashoffset: 0, duration: 0.32, ease: EASE.settle }, '-=0.22')
    tl.to(orb, { scale: 1.06, duration: 0.16, ease: 'power1.out' }).to(orb, { scale: 1, duration: 0.3, ease: EASE.settle })
  })
}

const CAPABILITIES = [
  { icon: 'role', title: '角色与按钮权限', desc: '菜单、按钮两级授权，前后端同一套权限码' },
  { icon: 'layers', title: '数据权限', desc: '全部 / 本部门及下级 / 本部门 / 本人' },
  { icon: 'log', title: '操作审计', desc: '登录日志与操作日志自动记录' },
  { icon: 'server', title: '边缘节点', desc: 'mTLS 注册、心跳与在线状态实时推送' },
]

async function refreshCaptcha() {
  captchaLoading.value = true
  try {
    const res = await authApi.captcha()
    captchaUuid.value = res.uuid
    captchaCode.value = res.code ?? ''
    captchaSvg.value = res.imageSvg ?? ''
  } catch {
    captchaUuid.value = ''
    captchaCode.value = ''
    captchaSvg.value = ''
    formError.value = '无法连接服务，请确认后端已启动'
  } finally {
    captchaLoading.value = false
  }
  form.captcha = ''
}

function redirectTarget() {
  const value = route.query.redirect
  return typeof value === 'string' && value.startsWith('/') ? value : '/'
}

/** 进入外壳；导航被取消（如菜单加载期间会话过期）时仍停在登录页并恢复按钮 */
async function enterShell(target: string) {
  await playSuccess()
  const failure = await router.replace(target)
  if (failure || router.currentRoute.value.name === 'login') succeeded.value = false
}

function beginMfa(challenge: MfaChallenge, target: string, rememberLogin: boolean) {
  mfaChallenge.value = challenge
  mfaTarget.value = target
  mfaRemember.value = rememberLogin
  mfaStep.value = true
  mfaCode.value = ''
  mfaRecovery.value = false
  mfaError.value = ''
  mfaFieldError.value = ''
  // 挑战已替代密码凭据；返回账号登录时重新输入口令与验证码
  form.password = ''
  form.captcha = ''
  void nextTick(() => document.querySelector<HTMLInputElement>('.mfa-code input')?.focus())
}

function toggleRecovery() {
  mfaRecovery.value = !mfaRecovery.value
  mfaCode.value = ''
  mfaError.value = ''
  mfaFieldError.value = ''
  void nextTick(() => document.querySelector<HTMLInputElement>('.mfa-code input')?.focus())
}

function backToPassword() {
  if (submitting.value) return
  mfaStep.value = false
  mfaChallenge.value = null
  mfaCode.value = ''
  mfaRecovery.value = false
  mfaError.value = ''
  mfaFieldError.value = ''
  form.password = ''
  void refreshCaptcha()
}

async function submitMfa() {
  mfaError.value = ''
  mfaFieldError.value = ''
  if (!mfaChallenge.value) {
    mfaError.value = '两步验证挑战已失效，请返回重新登录'
    return
  }
  const value = mfaCode.value.trim()
  if (mfaRecovery.value) {
    if (value.replace(/[^a-z0-9]/gi, '').length !== 8) {
      mfaFieldError.value = '请输入 8 位恢复码'
      return
    }
  } else if (!/^\d{6}$/.test(value)) {
    mfaFieldError.value = '请输入 6 位数字动态码'
    return
  }
  submitting.value = true
  try {
    await completeMfaLogin(mfaChallenge.value.challengeToken, value, mfaRemember.value)
    await enterShell(mfaTarget.value)
  } catch (error) {
    succeeded.value = false
    mfaError.value = error instanceof ApiError ? error.message : '两步验证失败，请稍后重试'
    mfaCode.value = ''
  } finally {
    submitting.value = false
  }
}

async function submit() {
  formError.value = ''
  errors.value = validate(form, {
    username: [required('请输入账号')],
    password: [required('请输入密码')],
    captcha: [required('请输入验证码')],
  })
  if (Object.keys(errors.value).length) return
  submitting.value = true
  try {
    const result = await login({ tenantCode: form.tenantCode.trim() || undefined, username: form.username.trim(), password: form.password, captcha: form.captcha.trim(), captchaUuid: captchaUuid.value }, remember.value)
    if (result.mfaRequired) {
      beginMfa(result, redirectTarget(), remember.value)
      return
    }
    await enterShell(redirectTarget())
  } catch (error) {
    succeeded.value = false
    formError.value = error instanceof ApiError ? error.message : '登录失败，请稍后重试'
    await refreshCaptcha()
  } finally {
    submitting.value = false
  }
}

function startOidc() {
  window.location.assign('/api/auth/oidc/start?return=' + encodeURIComponent(redirectTarget()))
}

/** OIDC 回调：用一次性 code 换会话，成功后进入原目标页；本地账号需第二因素时改走动态码。 */
async function completeOidc() {
  const code = typeof route.query.oidcCode === 'string' ? route.query.oidcCode : ''
  const errorText = typeof route.query.oidcError === 'string' ? route.query.oidcError : ''
  if (errorText) formError.value = errorText
  if (!code) return
  submitting.value = true
  try {
    const result = await loginWithOidc(code, true)
    if (result.mfaRequired) {
      beginMfa(result, result.returnPath || redirectTarget(), true)
      return
    }
    await enterShell(result.returnPath || redirectTarget())
  } catch (error) {
    succeeded.value = false
    formError.value = error instanceof ApiError ? error.message : '企业登录失败，请稍后重试'
  } finally {
    submitting.value = false
  }
}

/* ── 一次性开场：本次会话只播一次；任意点击或按键快进而不是硬切 ── */
const INTRO_KEY = 'scaffold.login-intro'
let intro: gsap.core.Timeline | null = null
const fastForward = () => intro?.timeScale(6)

function playIntro() {
  const el = root.value
  if (!el) return
  let seen = false
  try { seen = sessionStorage.getItem(INTRO_KEY) === '1' } catch { /* 忽略 */ }
  if (prefersReducedMotion() || document.hidden) {
    el.classList.add('is-entrance-ready')
    return
  }
  const q = (s: string) => el.querySelectorAll(s)
  intro = gsap.timeline({ paused: true, onComplete: () => { intro = null } })
  intro.set(['.login-brand', '.hero-line', '.cap-item', '.login-card', '.login-plate', '.login-dial'].flatMap(s => [...q(s)]), { opacity: 0, y: 16 })
  intro.set(q('.dial-tick'), { opacity: 0.15 })
  intro.add(() => el.classList.add('is-entrance-ready'))
  // 先对焦品牌 → 标题逐行落定 → 能力逐条接通 → 登录卡交付
  intro.to(q('.login-brand'), { opacity: 1, y: 0, duration: 0.7, ease: EASE.reveal }, 0)
  intro.to(q('.hero-line'), { opacity: 1, y: 0, duration: 0.9, ease: EASE.reveal, stagger: 0.08 }, 0.12)
  intro.to(q('.login-dial'), { opacity: 1, y: 0, duration: 0.9, ease: EASE.reveal }, 0.2)
  intro.to(q('.dial-tick'), { opacity: 1, duration: 0.3, ease: 'power1.out', stagger: { each: 0.008, from: 'start' } }, 0.3)
  intro.to(q('.cap-item'), { opacity: 1, y: 0, duration: 0.7, ease: EASE.reveal, stagger: { each: 0.06, from: 'start' } }, 0.36)
  intro.to(q('.login-plate'), { opacity: 1, y: 0, duration: 0.7, ease: EASE.reveal, stagger: 0.08 }, 0.5)
  intro.to(q('.login-card'), { opacity: 1, y: 0, duration: 0.8, ease: EASE.glide }, 0.22)
  intro.add(() => gsap.set(el.querySelectorAll('.login-brand, .hero-line, .cap-item, .login-plate, .login-dial'), { clearProps: 'transform' }))
  if (seen) intro.timeScale(2.4)
  try { sessionStorage.setItem(INTRO_KEY, '1') } catch { /* 忽略 */ }
  afterTwoFrames(() => intro?.play(0))
}

// 页面被切到后台时直接走完编排，避免停在隐藏帧
function onVisibility() {
  if (document.hidden && intro) intro.progress(1, false)
}

onMounted(() => {
  refreshCaptcha()
  probeHealth()
  authApi.oidcEnabled()
    .then(result => { oidcEnabled.value = Boolean(result.enabled) })
    .catch(() => { oidcEnabled.value = false })
  void completeOidc()
  clockTimer = window.setInterval(() => { if (!document.hidden) now.value = new Date() }, 1000)
  if (card.value && window.matchMedia('(hover: hover)').matches) {
    tiltX = gsap.quickTo(card.value, 'rotationX', { duration: 0.6, ease: 'power3.out' })
    tiltY = gsap.quickTo(card.value, 'rotationY', { duration: 0.6, ease: 'power3.out' })
  }
  playIntro()
  window.addEventListener('pointerdown', fastForward, { once: true })
  window.addEventListener('keydown', fastForward, { once: true })
  document.addEventListener('visibilitychange', onVisibility)
})
onBeforeUnmount(() => {
  window.clearInterval(clockTimer)
  window.removeEventListener('pointerdown', fastForward)
  window.removeEventListener('keydown', fastForward)
  document.removeEventListener('visibilitychange', onVisibility)
  intro?.kill()
})
</script>

<template>
  <div ref="root" class="login">
    <!-- 左：全幅深色画布 + 浮动玻璃面板 -->
    <section class="login-canvas" aria-label="系统介绍">
      <div class="canvas-grid" aria-hidden="true" />
      <div class="canvas-glow" aria-hidden="true" />
      <NeuralNet :burst-key="netBurst" />
      <div class="login-brand">
        <span class="brand-mark"><span class="brand-dot" /></span>
        <div>
          <div class="brand-name">{{ APP.name }}</div>
          <div class="brand-sub">{{ APP.subtitle }}</div>
        </div>
      </div>

      <div class="canvas-main">
        <div class="canvas-copy">
          <span class="eyebrow hero-line">通用后台底座 · {{ APP.version }}</span>
          <h1 class="login-title">
            <span class="hero-line">统一的后台底座，</span>
            <span class="hero-line">专注业务本身。</span>
          </h1>
          <p class="login-lead hero-line">用户、角色、菜单、部门、字典与审计开箱即用，新业务模块只需关注字段与流程。</p>
        </div>

        <!-- 实时时钟：秒针对应的刻度点亮，逐秒推进（导 Conduct） -->
        <div class="login-dial" aria-hidden="true">
          <svg viewBox="0 0 100 100">
            <line
              v-for="t in TICKS"
              :key="t.i"
              :class="['dial-tick', { 'is-major': t.major, 'is-on': t.i <= now.getSeconds() }]"
              :x1="t.x1" :y1="t.y1" :x2="t.x2" :y2="t.y2"
            />
          </svg>
          <div class="dial-center">
            <span class="dial-time tnum">{{ clock }}</span>
            <span class="dial-date">{{ dateLine }}</span>
          </div>
        </div>
      </div>

      <div class="canvas-plates">
        <div class="login-plate is-health">
          <span class="eyebrow">服务状态</span>
          <span :class="['health-dot', { 'is-ok': health.ok }]" aria-hidden="true" />
          <b>{{ health.ok ? '运行正常' : health.latency === null ? '连接中断' : '状态异常' }}</b>
          <span class="tnum plate-sub">{{ health.latency === null ? '—' : `${health.latency} ms` }}</span>
        </div>
        <ul class="login-plate cap-list">
          <li v-for="cap in CAPABILITIES" :key="cap.title" class="cap-item">
            <span class="cap-ico"><Icon :name="cap.icon" :size="16" /></span>
            <span>
              <span class="cap-title">{{ cap.title }}</span>
              <span class="cap-desc">{{ cap.desc }}</span>
            </span>
          </li>
        </ul>
      </div>
      <div class="login-foot">{{ APP.copyright }}</div>
    </section>

    <!-- 右：登录卡 -->
    <section class="login-side" @pointermove="onTilt" @pointerleave="resetTilt">
      <div ref="card" class="login-card m-float m-glow" role="region" aria-labelledby="login-heading" @pointermove="trackGlow">
        <span class="glow-layer" aria-hidden="true" />
        <div class="login-card-inner">
          <span class="eyebrow">{{ mfaStep ? '第二因素' : '账号登录' }}</span>
          <h2 id="login-heading" class="login-card-title">{{ mfaStep ? '两步验证' : '登录' }}</h2>
          <p class="login-card-sub">
            {{ mfaStep ? (mfaRecovery ? '输入一枚恢复码，完成本次登录' : '输入身份验证器中的 6 位动态码') : '使用管理员分配的账号登录系统' }}
          </p>

          <form v-if="!mfaStep" class="login-form" novalidate @submit.prevent="submit">
            <Field label="租户编码">
              <TextInput v-model="form.tenantCode" icon="dept" placeholder="默认租户可留空" autocomplete="organization" />
            </Field>
            <Field label="账号" :error="errors.username">
              <TextInput v-model="form.username" icon="user" placeholder="请输入账号" autocomplete="username" :invalid="!!errors.username" />
            </Field>
            <Field label="密码" :error="errors.password">
              <TextInput v-model="form.password" type="password" icon="lock" placeholder="请输入密码" autocomplete="current-password" :invalid="!!errors.password" />
            </Field>
            <Field label="验证码" :error="errors.captcha">
              <div class="captcha-row">
                <TextInput v-model="form.captcha" icon="shield" placeholder="请输入右侧字符" :maxlength="6" autocomplete="off" :invalid="!!errors.captcha" />
                <button type="button" class="captcha-box m-groove" :disabled="captchaLoading" aria-label="看不清，换一张" title="看不清，换一张" @click="refreshCaptcha">
                  <img v-if="captchaImage" class="captcha-image" :src="captchaImage" alt="验证码" />
                  <span v-else-if="captchaCode" class="captcha-code" aria-hidden="true">
                    <span v-for="(ch, i) in captchaCode.split('')" :key="i" :style="{ rotate: `${(i % 2 ? 1 : -1) * (6 + i * 3)}deg` }">{{ ch }}</span>
                  </span>
                  <Icon v-else name="refresh" :size="16" />
                </button>
              </div>
            </Field>
            <div class="login-options">
              <Checkbox v-model="remember" label="记住登录状态" />
            </div>
            <p v-if="formError" class="login-error" role="alert"><Icon name="alert" :size="15" />{{ formError }}</p>
            <div class="login-submit-slot">
              <Button v-if="!succeeded" type="submit" variant="primary" block :loading="submitting" class="login-submit">登录</Button>
              <div v-else class="success-orb" role="status" aria-label="登录成功，正在进入">
                <svg width="30" height="30" viewBox="0 0 46 46" aria-hidden="true">
                  <circle cx="23" cy="23" r="20" stroke-dasharray="126" />
                  <path d="m15 23.5 5.5 5.5L31.5 18" stroke-dasharray="24" />
                </svg>
              </div>
            </div>
            <Button v-if="oidcEnabled && !succeeded" type="button" variant="secondary" block class="login-oidc" :disabled="submitting" @click="startOidc">
              企业登录
            </Button>
          </form>

          <form v-else class="login-form" novalidate @submit.prevent="submitMfa">
            <Field :label="mfaRecovery ? '恢复码' : '动态验证码'" :error="mfaFieldError">
              <div class="mfa-code">
                <TextInput
                  v-model="mfaCode"
                  icon="shield"
                  :placeholder="mfaRecovery ? '请输入 8 位恢复码' : '请输入 6 位动态码'"
                  :maxlength="mfaRecovery ? 12 : 6"
                  autocomplete="one-time-code"
                  :invalid="!!mfaFieldError"
                  :disabled="submitting"
                />
              </div>
            </Field>
            <p v-if="mfaError" class="login-error" role="alert"><Icon name="alert" :size="15" />{{ mfaError }}</p>
            <div class="login-options">
              <button type="button" class="link-btn" :disabled="submitting" @click="toggleRecovery">
                {{ mfaRecovery ? '使用身份验证器' : '使用恢复码' }}
              </button>
              <span class="faint" style="font-size: 12px">账号已启用两步验证</span>
            </div>
            <div class="login-submit-slot">
              <Button v-if="!succeeded" type="submit" variant="primary" block :loading="submitting" class="login-submit">验证并登录</Button>
              <div v-else class="success-orb" role="status" aria-label="登录成功，正在进入">
                <svg width="30" height="30" viewBox="0 0 46 46" aria-hidden="true">
                  <circle cx="23" cy="23" r="20" stroke-dasharray="126" />
                  <path d="m15 23.5 5.5 5.5L31.5 18" stroke-dasharray="24" />
                </svg>
              </div>
            </div>
            <Button v-if="!succeeded" type="button" variant="ghost" block :disabled="submitting" @click="backToPassword">
              返回账号登录
            </Button>
          </form>
        </div>
      </div>
    </section>
  </div>
</template>
