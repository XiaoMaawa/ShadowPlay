// app/service-const.cjs — 服务端常量（主进程 / preload / 前端共用）
//
// 采集服务是独立常驻进程，界面只负责"连上它"，所以双方约定一个固定会话 id。
// 必须与 native/backend/RingNames.h 里的 kServiceSessionId() 完全一致。
module.exports = {
  SERVICE_SESSION_ID: 'shadowplay-service-v1',
  BACKEND_EXE: 'shadow-capture.exe',
}
