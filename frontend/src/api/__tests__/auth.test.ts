import test from 'node:test'
import assert from 'node:assert/strict'
import { createHttpClient } from '../http.ts'
import { createAuthApi } from '../auth.ts'

test('auth api maps login, current user, routes and logout endpoints', async () => {
  const calls: Array<{ url: string; method: string; body?: string }> = []
  const fetcher = async (url: string | URL | Request, init?: RequestInit) => {
    calls.push({ url: String(url), method: init?.method ?? 'GET', body: init?.body as string | undefined })
    const data = url.toString().endsWith('/login')
      ? { token: 'jwt-token', expiresIn: 7200 }
      : url.toString().endsWith('/me')
        ? { username: 'admin', nickname: '周明远', roles: ['super_admin'], permissions: ['system:user:list'] }
        : url.toString().endsWith('/routes')
          ? { routes: [{ name: '工作台', path: '/dashboard' }] }
          : null
    return new Response(JSON.stringify({ code: 200, message: 'success', data, traceId: 't' }), {
      status: 200,
      headers: { 'content-type': 'application/json' },
    })
  }
  const api = createAuthApi(createHttpClient({ baseUrl: '/api', tokenProvider: () => 'token', fetcher }))

  const login = await api.login({ username: 'admin', password: 'admin123', captcha: '1234', captchaUuid: 'uuid-1' })
  const me = await api.me()
  const routes = await api.routes()
  await api.changePassword({ oldPassword: 'oldPwd123', newPassword: 'newPwd123' })
  await api.logout()

  assert.equal(login.token, 'jwt-token')
  assert.equal(me.username, 'admin')
  assert.deepEqual(routes.routes, [{ name: '工作台', path: '/dashboard' }])
  assert.deepEqual(calls.map(call => `${call.method} ${call.url}`), [
    'POST /api/auth/login',
    'GET /api/auth/me',
    'GET /api/auth/routes',
    'PUT /api/auth/password',
    'POST /api/auth/logout',
  ])
  assert.deepEqual(JSON.parse(calls[0].body ?? '{}'), { username: 'admin', password: 'admin123', captcha: '1234', captchaUuid: 'uuid-1' })
  assert.deepEqual(JSON.parse(calls[3].body ?? '{}'), { oldPassword: 'oldPwd123', newPassword: 'newPwd123' })
})

test('auth api maps two-factor challenge and MFA management endpoints', async () => {
  const calls: Array<{ url: string; method: string; body?: string }> = []
  const fetcher = async (url: string | URL | Request, init?: RequestInit) => {
    calls.push({ url: String(url), method: init?.method ?? 'GET', body: init?.body as string | undefined })
    const path = new URL(String(url), 'http://localhost').pathname
    const data =
      path === '/api/auth/login'
        ? { mfaRequired: true, challengeToken: 'challenge-1', expiresIn: 300 }
        : path === '/api/auth/oidc/exchange'
          ? { mfaRequired: true, challengeToken: 'oidc-challenge', expiresIn: 300, returnPath: '/device/nodes' }
          : path === '/api/auth/mfa'
            ? { enabled: true, recoveryCodesRemaining: 7 }
            : path === '/api/auth/mfa/setup'
              ? { secret: 'JBSWY3DPEHPK3PXP', otpauthUri: 'otpauth://totp/EdgePulse:admin?secret=JBSWY3DPEHPK3PXP' }
              : path === '/api/auth/mfa/enable'
                ? { recoveryCodes: ['abcd-efgh', 'ijkl-mnop'] }
                : path === '/api/auth/mfa/verify'
                  ? { token: 'mfa-token', expiresIn: 7200 }
                  : path === '/api/auth/mfa/recovery-codes'
                    ? { recoveryCodes: ['qrst-uvwx'] }
                    : null
    return new Response(JSON.stringify({ code: 200, message: 'success', data, traceId: 't' }), {
      status: 200,
      headers: { 'content-type': 'application/json' },
    })
  }
  const api = createAuthApi(createHttpClient({ baseUrl: '/api', tokenProvider: () => 'token', fetcher }))

  const login = await api.login({ username: 'admin', password: 'admin123', captcha: '1234', captchaUuid: 'uuid-1' })
  const oidc = await api.oidcExchange({ code: 'oidc-code' })
  const status = await api.mfaStatus()
  const setup = await api.setupMfa({ password: 'admin123' })
  const enabled = await api.enableMfa({ password: 'admin123', code: '123456' })
  await api.disableMfa({ password: 'admin123', code: '654321' })
  const verified = await api.verifyMfa({ challengeToken: 'challenge-1', code: '654321' })
  const regenerated = await api.recoveryCodes({ code: '123456' })

  assert.equal(login.token, undefined)
  assert.equal(login.mfaRequired, true)
  assert.equal(login.challengeToken, 'challenge-1')
  assert.equal(login.expiresIn, 300)
  assert.equal(oidc.mfaRequired, true)
  assert.equal(oidc.challengeToken, 'oidc-challenge')
  assert.equal(oidc.returnPath, '/device/nodes')
  assert.deepEqual(status, { enabled: true, recoveryCodesRemaining: 7 })
  assert.deepEqual(setup, { secret: 'JBSWY3DPEHPK3PXP', otpauthUri: 'otpauth://totp/EdgePulse:admin?secret=JBSWY3DPEHPK3PXP' })
  assert.deepEqual(enabled.recoveryCodes, ['abcd-efgh', 'ijkl-mnop'])
  assert.deepEqual(verified, { token: 'mfa-token', expiresIn: 7200 })
  assert.deepEqual(regenerated.recoveryCodes, ['qrst-uvwx'])

  assert.deepEqual(calls.map(call => `${call.method} ${call.url}`), [
    'POST /api/auth/login',
    'POST /api/auth/oidc/exchange',
    'GET /api/auth/mfa',
    'POST /api/auth/mfa/setup',
    'POST /api/auth/mfa/enable',
    'POST /api/auth/mfa/disable',
    'POST /api/auth/mfa/verify',
    'POST /api/auth/mfa/recovery-codes',
  ])
  assert.deepEqual(JSON.parse(calls[1].body ?? '{}'), { code: 'oidc-code' })
  assert.deepEqual(JSON.parse(calls[3].body ?? '{}'), { password: 'admin123' })
  assert.deepEqual(JSON.parse(calls[4].body ?? '{}'), { password: 'admin123', code: '123456' })
  assert.deepEqual(JSON.parse(calls[5].body ?? '{}'), { password: 'admin123', code: '654321' })
  assert.deepEqual(JSON.parse(calls[6].body ?? '{}'), { challengeToken: 'challenge-1', code: '654321' })
  assert.deepEqual(JSON.parse(calls[7].body ?? '{}'), { code: '123456' })
})
