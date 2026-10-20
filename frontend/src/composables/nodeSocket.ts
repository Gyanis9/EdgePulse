import { onBeforeUnmount, onMounted, ref } from 'vue'
import { authSession, type NodeRow } from '@/api'

export type NodeSocketState = 'connecting' | 'connected' | 'disconnected'

interface NodeStatusMessage {
  type: 'node.status'
  data: NodeRow
}

const RECONNECT_BASE_MILLISECONDS = 1000
const RECONNECT_MAX_MILLISECONDS = 30000
const HEARTBEAT_MILLISECONDS = 25000

/**
 * 浏览器节点状态推送：首条消息认证、指数退避重连、页面隐藏时暂停心跳、卸载时关闭。
 */
export function useNodeSocket(onStatus: (node: NodeRow) => void) {
  const state = ref<NodeSocketState>('disconnected')
  let socket: WebSocket | null = null
  let reconnectTimer = 0
  let heartbeatTimer = 0
  let reconnectAttempts = 0
  let disposed = false

  function socketUrl() {
    const scheme = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    return `${scheme}//${window.location.host}/api/ws`
  }

  function stopHeartbeat() {
    if (heartbeatTimer) {
      window.clearInterval(heartbeatTimer)
      heartbeatTimer = 0
    }
  }

  function startHeartbeat() {
    stopHeartbeat()
    heartbeatTimer = window.setInterval(() => {
      if (socket?.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ type: 'ping' }))
      }
    }, HEARTBEAT_MILLISECONDS)
  }

  function scheduleReconnect() {
    if (disposed || document.hidden) return
    const delay = Math.min(RECONNECT_MAX_MILLISECONDS, RECONNECT_BASE_MILLISECONDS * 2 ** Math.min(reconnectAttempts, 5))
    reconnectAttempts += 1
    reconnectTimer = window.setTimeout(connect, delay)
  }

  function connect() {
    if (disposed) return
    if (socket && (socket.readyState === WebSocket.CONNECTING || socket.readyState === WebSocket.OPEN)) return
    const token = authSession.token()
    if (!token) {
      state.value = 'disconnected'
      return
    }
    state.value = 'connecting'
    socket = new WebSocket(socketUrl())
    socket.onopen = () => {
      // 令牌只放在首条消息里，不拼进 URL
      socket?.send(JSON.stringify({ type: 'auth', token }))
    }
    socket.onmessage = event => {
      let message: Record<string, unknown>
      try {
        message = JSON.parse(String(event.data)) as Record<string, unknown>
      } catch {
        return
      }
      if (message.type === 'auth' && message.ok === true) {
        state.value = 'connected'
        reconnectAttempts = 0
        startHeartbeat()
        return
      }
      if (message.type === 'node.status' && message.data && typeof message.data === 'object') {
        onStatus(message.data as NodeRow)
      }
    }
    socket.onerror = () => {
      socket?.close()
    }
    socket.onclose = () => {
      stopHeartbeat()
      socket = null
      state.value = 'disconnected'
      scheduleReconnect()
    }
  }

  function handleVisibility() {
    if (document.hidden) {
      // 页面隐藏时停心跳；连接仍在，服务端空闲超时后由可见时的重连接管
      stopHeartbeat()
      return
    }
    if (socket?.readyState === WebSocket.OPEN) startHeartbeat()
    else connect()
  }

  onMounted(() => {
    connect()
    document.addEventListener('visibilitychange', handleVisibility)
  })

  onBeforeUnmount(() => {
    disposed = true
    document.removeEventListener('visibilitychange', handleVisibility)
    window.clearTimeout(reconnectTimer)
    stopHeartbeat()
    socket?.close()
    socket = null
  })

  return { state }
}
