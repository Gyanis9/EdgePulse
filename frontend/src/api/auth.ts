import type { HttpClient } from './http'

export interface LoginRequest {
  username: string
  password: string
  captcha: string
  captchaUuid: string
  tenantCode?: string
}

export interface CaptchaResponse {
  uuid: string
  code?: string
  imageSvg?: string
}

export interface LoginResponse {
  /** 需要第二因素时不下发会话令牌，只返回挑战 */
  token?: string
  expiresIn: number
  mfaRequired?: boolean
  challengeToken?: string
}

/** 密码或 OIDC 登录返回的第二因素挑战；成功校验后即消费 */
export interface MfaChallenge {
  mfaRequired: true
  challengeToken: string
  expiresIn: number
}

export interface MfaStatusResponse {
  enabled: boolean
  recoveryCodesRemaining: number
}

export interface MfaSetupResponse {
  secret: string
  otpauthUri: string
}

export interface MfaRecoveryCodesResponse {
  recoveryCodes: string[]
}

export interface MfaVerifyResponse {
  token: string
  expiresIn: number
}

export interface ChangePasswordRequest {
  oldPassword: string
  newPassword: string
}

export interface CurrentUser {
  id?: number
  username: string
  nickname: string
  deptId?: number
  deptName?: string
  tenantId?: number
  tenantCode?: string
  tenantName?: string
  platformAdmin?: boolean
  roles: string[]
  permissions: string[]
}

export interface RouteItem {
  id?: number
  parentId?: number
  name: string
  routeName?: string
  routePath?: string
  path?: string
  component?: string
  icon?: string
  permission?: string
  children?: RouteItem[]
}

export interface RoutesResponse {
  routes: RouteItem[]
}

export interface OidcEnabledResponse {
  enabled: boolean
  label: string
}

export interface OidcExchangeResponse {
  /** 需要第二因素时不下发会话令牌，只返回挑战 */
  token?: string
  expiresIn: number
  returnPath: string
  mfaRequired?: boolean
  challengeToken?: string
}

export function createAuthApi(client: HttpClient) {
  return {
    captcha: () => client.get<CaptchaResponse>('/auth/captcha'),
    login: (body: LoginRequest) => client.post<LoginResponse>('/auth/login', body),
    me: () => client.get<CurrentUser>('/auth/me'),
    routes: () => client.get<RoutesResponse>('/auth/routes'),
    changePassword: (body: ChangePasswordRequest) => client.put<void>('/auth/password', body),
    logout: () => client.post<void>('/auth/logout'),
    oidcEnabled: () => client.get<OidcEnabledResponse>('/auth/oidc/enabled'),
    oidcExchange: (body: { code: string }) => client.post<OidcExchangeResponse>('/auth/oidc/exchange', body),
    mfaStatus: () => client.get<MfaStatusResponse>('/auth/mfa'),
    setupMfa: (body: { password: string }) => client.post<MfaSetupResponse>('/auth/mfa/setup', body),
    enableMfa: (body: { password: string; code: string }) => client.post<MfaRecoveryCodesResponse>('/auth/mfa/enable', body),
    disableMfa: (body: { password: string; code: string }) => client.post<void>('/auth/mfa/disable', body),
    verifyMfa: (body: { challengeToken: string; code: string }) => client.post<MfaVerifyResponse>('/auth/mfa/verify', body),
    recoveryCodes: (body: { code: string }) => client.post<MfaRecoveryCodesResponse>('/auth/mfa/recovery-codes', body),
  }
}
