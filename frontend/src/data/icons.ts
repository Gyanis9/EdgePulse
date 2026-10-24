/* 线性图标：24 网格、1.6 描边、圆头圆角。菜单表 sys_menu.icon 的取值必须在此登记。 */
export type IconNode = readonly [string, Readonly<Record<string, string | number>>]
export type IconDefinition = string | readonly IconNode[]

export const ICON_PATHS: Record<string, IconDefinition> = {
  // 导航
  dashboard: 'M4 13h6V4H4v9zm0 7h6v-4H4v4zm10 0h6v-9h-6v9zm0-16v4h6V4h-6z',
  gear: 'M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6zm7.4-3a7.4 7.4 0 0 0-.1-1.3l2-1.6-2-3.4-2.4 1a7.6 7.6 0 0 0-2.2-1.3L14.4 3h-4.8l-.3 2.4a7.6 7.6 0 0 0-2.2 1.3l-2.4-1-2 3.4 2 1.6a7.4 7.4 0 0 0 0 2.6l-2 1.6 2 3.4 2.4-1c.7.6 1.4 1 2.2 1.3l.3 2.4h4.8l.3-2.4c.8-.3 1.5-.7 2.2-1.3l2.4 1 2-3.4-2-1.6c.1-.4.1-.9.1-1.3z',
  users: 'M9 11a4 4 0 1 0 0-8 4 4 0 0 0 0 8zm-6 10v-1a6 6 0 0 1 12 0v1m1-10a3 3 0 1 0 0-6m2 16v-1a5 5 0 0 0-3-4.6',
  user: 'M12 12a4 4 0 1 0 0-8 4 4 0 0 0 0 8zm-7 9v-1a7 7 0 0 1 14 0v1',
  role: 'M12 3 5 6v5c0 4.4 3 8.4 7 10 4-1.6 7-5.6 7-10V6l-7-3zm-3 9 2 2 4-4',
  shield: 'M12 3 5 6v5c0 4.4 3 8.4 7 10 4-1.6 7-5.6 7-10V6l-7-3z',
  menu: 'M4 6h16M4 12h16M4 18h10',
  dept: 'M12 3v5m-7 4V9h14v3M5 21v-6H2v6h3zm8.5 0v-6h-3v6h3zM22 21v-6h-3v6h3z',
  post: 'M8 7V5a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M3 8h18v11H3V8zm0 5h18',
  dict: 'M5 4.5A1.5 1.5 0 0 1 6.5 3H19v15H6.5A1.5 1.5 0 0 0 5 19.5v-15zm0 15A1.5 1.5 0 0 0 6.5 21H19M9 7h6m-6 4h4',
  log: 'M6 3h9l4 4v14H6V3zm8 0v5h5M9 12h7m-7 4h5',
  file: 'M6 3h9l4 4v14H6V3zm8 0v5h5',
  // Lucide 兼容的菜单图标（sys_menu.icon 直接使用这些名字）
  // 以下多节点图标取自 Lucide 1.61（ISC，Copyright (c) Lucide Contributors）
  book: [['path', { d: 'M4 19.5v-15A2.5 2.5 0 0 1 6.5 2H19a1 1 0 0 1 1 1v18a1 1 0 0 1-1 1H6.5a1 1 0 0 1 0-5H20' }]],
  briefcase: [
    ['path', { d: 'M16 20V4a2 2 0 0 0-2-2h-4a2 2 0 0 0-2 2v16' }],
    ['rect', { width: '20', height: '14', x: '2', y: '6', rx: '2' }],
  ],
  building: [
    ['path', { d: 'M12 10h.01' }],
    ['path', { d: 'M12 14h.01' }],
    ['path', { d: 'M12 6h.01' }],
    ['path', { d: 'M16 10h.01' }],
    ['path', { d: 'M16 14h.01' }],
    ['path', { d: 'M16 6h.01' }],
    ['path', { d: 'M8 10h.01' }],
    ['path', { d: 'M8 14h.01' }],
    ['path', { d: 'M8 6h.01' }],
    ['path', { d: 'M9 22v-3a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v3' }],
    ['rect', { x: '4', y: '2', width: '16', height: '20', rx: '2' }],
  ],
  'building-2': [
    ['path', { d: 'M10 12h4' }],
    ['path', { d: 'M10 8h4' }],
    ['path', { d: 'M14 21v-3a2 2 0 0 0-4 0v3' }],
    ['path', { d: 'M6 10H4a2 2 0 0 0-2 2v7a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-2' }],
    ['path', { d: 'M6 21V5a2 2 0 0 1 2-2h8a2 2 0 0 1 2 2v16' }],
  ],
  'layout-dashboard': [
    ['rect', { width: '7', height: '9', x: '3', y: '3', rx: '1' }],
    ['rect', { width: '7', height: '5', x: '14', y: '3', rx: '1' }],
    ['rect', { width: '7', height: '9', x: '14', y: '12', rx: '1' }],
    ['rect', { width: '7', height: '5', x: '3', y: '16', rx: '1' }],
  ],
  'line-chart': [
    ['path', { d: 'M3 3v16a2 2 0 0 0 2 2h16' }],
    ['path', { d: 'm19 9-5 5-4-4-3 3' }],
  ],
  'list-tree': [
    ['path', { d: 'M8 5h13' }],
    ['path', { d: 'M13 12h8' }],
    ['path', { d: 'M13 19h8' }],
    ['path', { d: 'M3 10a2 2 0 0 0 2 2h3' }],
    ['path', { d: 'M3 5v12a2 2 0 0 0 2 2h3' }],
  ],
  'log-in': [
    ['path', { d: 'm10 17 5-5-5-5' }],
    ['path', { d: 'M15 12H3' }],
    ['path', { d: 'M15 3h4a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-4' }],
  ],
  package: [
    ['path', { d: 'M11 21.73a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73z' }],
    ['path', { d: 'M12 22V12' }],
    ['polyline', { points: '3.29 7 12 12 20.71 7' }],
    ['path', { d: 'm7.5 4.27 9 5.15' }],
  ],
  'scroll-text': [
    ['path', { d: 'M15 12h-5' }],
    ['path', { d: 'M15 8h-5' }],
    ['path', { d: 'M19 17V5a2 2 0 0 0-2-2H4' }],
    ['path', { d: 'M8 21h12a2 2 0 0 0 2-2v-1a1 1 0 0 0-1-1H11a1 1 0 0 0-1 1v1a2 2 0 1 1-4 0V5a2 2 0 1 0-4 0v2a1 1 0 0 0 1 1h3' }],
  ],
  settings: [
    ['path', { d: 'M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915' }],
    ['circle', { cx: '12', cy: '12', r: '3' }],
  ],
  terminal: [
    ['path', { d: 'M12 19h8' }],
    ['path', { d: 'm4 17 6-6-6-6' }],
  ],
  send: [
    ['path', { d: 'M14.536 21.686a.5.5 0 0 0 .937-.024l6.5-19a.496.496 0 0 0-.635-.635l-19 6.5a.5.5 0 0 0-.024.937l7.93 3.18a2 2 0 0 1 1.112 1.11z' }],
    ['path', { d: 'm21.854 2.147-10.94 10.939' }],
  ],
  lock: 'M7 11V8a5 5 0 0 1 10 0v3M5 11h14v10H5V11zm7 4v2',
  tool: 'M14.7 6.3a4 4 0 0 0-5.1 5.1L3.5 17.5l3 3 6.1-6.1a4 4 0 0 0 5.1-5.1l-2.4 2.4-2.6-.4-.4-2.6 2.4-2.4z',
  command: 'M8 16V8m8 8V8M8 8a2 2 0 1 1 2-2v12a2 2 0 1 1-2-2h8a2 2 0 1 1-2 2V6a2 2 0 1 1 2 2H8',
  code: 'm8 7-5 5 5 5m8-10 5 5-5 5M14 4l-4 16',
  server: 'M4 4h16v6H4V4zm0 10h16v6H4v-6zm3-7h2m-2 10h2',
  database: 'M4 6c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3zm0 0v12c0 1.7 3.6 3 8 3s8-1.3 8-3V6m-16 6c0 1.7 3.6 3 8 3s8-1.3 8-3',
  monitor: 'M3 4h18v12H3V4zm6 17h6m-3-5v5',
  layers: 'M12 3 3 8l9 5 9-5-9-5zm-9 9 9 5 9-5m-18 4 9 5 9-5',
  grid: 'M4 4h6v6H4V4zm10 0h6v6h-6V4zM4 14h6v6H4v-6zm10 0h6v6h-6v-6z',
  home: 'M4 11 12 4l8 7v9h-5v-6H9v6H4v-9z',
  chart: 'M4 20V10m6 10V4m6 16v-7m4 7H3',
  activity: 'M3 12h4l3-7 4 14 3-7h4',
  clock: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zm0-13v4l3 2',
  bell: 'M18 16V11a6 6 0 0 0-12 0v5l-2 2h16l-2-2zm-8 4a2 2 0 0 0 4 0',
  mail: 'M3 5h18v14H3V5zm0 1 9 7 9-7',
  phone: 'M8 3h8a1 1 0 0 1 1 1v16a1 1 0 0 1-1 1H8a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1zm3 15h2',
  key: 'M14 10a5 5 0 1 0-4 4.9V17h2v2h2v2h3v-3l-3-3v-.1A5 5 0 0 0 14 10zm-6-1h.01',
  sliders: 'M4 6h10m4 0h2M4 12h4m4 0h8M4 18h12m4 0h0M14 4v4M8 10v4m8 2v4',
  filter: 'M4 5h16l-6 8v6l-4-2v-4L4 5z',

  // 动作
  plus: 'M12 5v14M5 12h14',
  minus: 'M5 12h14',
  fit: 'M4 9V5a1 1 0 0 1 1-1h4M15 4h4a1 1 0 0 1 1 1v4M20 15v4a1 1 0 0 1-1 1h-4M9 20H5a1 1 0 0 1-1-1v-4',
  board: 'M4 5h4v14H4zM10 5h4v9h-4zM16 5h4v11h-4z',
  matrix: 'M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h6v6h-6z',
  table: 'M4 5h16v14H4zM4 10h16M4 15h16M10 5v14',
  edit: 'M4 20h4L19 9a2.8 2.8 0 0 0-4-4L4 16v4zm9.5-13.5 4 4',
  trash: 'M4 7h16M10 11v6m4-6v6M6 7l1 13h10l1-13M9 7V4h6v3',
  play: 'M8 5.5v13l10.5-6.5L8 5.5z',
  pause: 'M8 5v14m8-14v14',
  refresh: 'M20 11a8 8 0 0 0-14.9-3M4 4v4h4m-4 5a8 8 0 0 0 14.9 3M20 20v-4h-4',
  download: 'M12 4v11m0 0-4.5-4.5M12 15l4.5-4.5M5 20h14',
  upload: 'M12 20V9m0 0-4.5 4.5M12 9l4.5 4.5M5 4h14',
  search: 'M11 18a7 7 0 1 0 0-14 7 7 0 0 0 0 14zm9 2-4-4',
  eye: 'M2 12s3.6-7 10-7 10 7 10 7-3.6 7-10 7S2 12 2 12zm10 3a3 3 0 1 0 0-6 3 3 0 0 0 0 6z',
  copy: 'M9 9h11v11H9V9zM5 15H4V4h11v1',
  check: 'm5 12.5 4.5 4.5L19 7.5',
  x: 'M6 6l12 12M18 6 6 18',
  logout: 'M15 4h4v16h-4M10 8l-4 4 4 4m-4-4h10',
  more: 'M5 12h.01M12 12h.01M19 12h.01',
  power: 'M12 3v8m5.7-4.7a8 8 0 1 1-11.4 0',
  ban: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM5.6 5.6l12.8 12.8',

  // 方向
  chevronRight: 'm9 6 6 6-6 6',
  chevronLeft: 'm15 6-6 6 6 6',
  chevronDown: 'm6 9 6 6 6-6',
  arrowUp: 'M12 19V5m0 0-6 6m6-6 6 6',
  arrowDown: 'M12 5v14m0 0 6-6m-6 6-6-6',
  arrowRight: 'M5 12h14m0 0-6-6m6 6-6 6',
  sidebar: 'M4 4h16v16H4V4zm5 0v16',

  // 状态与外观
  info: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zm0-10v5m0-8h.01',
  alert: 'M12 3 2 20h20L12 3zm0 6v5m0 3h.01',
  sun: 'M12 16a4 4 0 1 0 0-8 4 4 0 0 0 0 8zm0-14v2m0 16v2M4.9 4.9l1.4 1.4m11.4 11.4 1.4 1.4M2 12h2m16 0h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4',
  moon: 'M20 14.5A8 8 0 0 1 9.5 4 8 8 0 1 0 20 14.5z',
  contrast: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zm0-18v18',
}

/** 菜单图标可选项（菜单管理表单使用） */
export const MENU_ICON_OPTIONS = [
  'dashboard', 'gear', 'users', 'user', 'role', 'shield', 'menu', 'dept', 'post', 'dict', 'log', 'file',
  'lock', 'tool', 'command', 'code', 'database', 'monitor', 'layers', 'grid', 'home', 'chart', 'activity',
  'clock', 'bell', 'mail', 'key', 'sliders', 'book', 'briefcase', 'building', 'building-2', 'layout-dashboard',
  'line-chart', 'list-tree', 'log-in', 'package', 'scroll-text', 'settings', 'terminal', 'send',
]
