// 在样式加载前写入外观，避免深色模式首屏闪白。
// 独立成文件，避免内联脚本被服务端的 CSP（script-src 'self'）拒绝。
try {
  var saved = JSON.parse(localStorage.getItem('scaffold.appearance') || '{}')
  var theme = saved.theme === 'dark' || (saved.theme === 'auto' && matchMedia('(prefers-color-scheme: dark)').matches) ? 'dark' : 'light'
  document.documentElement.dataset.theme = theme
  document.documentElement.dataset.density = saved.density || 'regular'
} catch (e) {}
