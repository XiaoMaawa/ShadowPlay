// frontend/src/lib/frameRenderer.ts
//
// 把共享内存里的 BGRA 帧直接用 WebGL2 画到 canvas 上。
//
// 关键点：
//   * 后端给的是 DXGI_FORMAT_B8G8R8A8_UNORM、自上而下、每行 stride 字节。
//     WebGL2 只认 RGBA 字节序，所以给纹素加 .bgra 后缀让采样时自动换序，
//     这样就不用每帧在 CPU 上做一次 swizzle（那是 33MB/帧的额外拷贝）。
//   * 整帧直接上传，不经过任何中间 Uint8ClampedArray。
//   * 采样坐标翻转一次即可匹配"第 0 行在屏幕最上面"的布局。

const VERTEX_SRC = `#version 300 es
in vec2 a_pos;
uniform vec2 u_scale;
out vec2 v_uv;
void main() {
  v_uv = vec2(a_pos.x, 1.0 - a_pos.y);
  // u_scale 把 [0,1] 的四边形在归一化设备坐标里缩到目标矩形，**居中**摆放。
  // u_scale=1 就是铺满视口；小于 1 的那一对轴自然留黑边（letterbox）。
  gl_Position = vec4((a_pos * u_scale + (1.0 - u_scale) * 0.5) * 2.0 - 1.0, 0.0, 1.0);
}`

const FRAGMENT_SRC = `#version 300 es
precision mediump float;
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 outColor;
void main() {
  // 后端给的是 BGRA 字节序；WebGL2 只能按 RGBA 读，所以 R 通道里其实是蓝、
  // B 通道里其实是红，采样后换一下即可。
  //
  // 不要写成 vec4(texture(u_tex, v_uv).bgra, 1.0)：把字段选择器放进构造函数
  // 参数位置时，有些驱动会把它当成分量列表展开，报
  // "constructor too many arguments"。显式取名再组合最稳。
  vec4 c = texture(u_tex, v_uv);
  outColor = vec4(c.b, c.g, c.r, 1.0);
}`

function compile(gl: WebGL2RenderingContext, type: number, src: string): WebGLShader {
  const shader = gl.createShader(type)
  if (!shader) throw new Error('createShader failed')
  gl.shaderSource(shader, src)
  gl.compileShader(shader)
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(shader)
    gl.deleteShader(shader)
    throw new Error(`shader compile failed: ${log}`)
  }
  return shader
}

export interface FrameGeometry {
  width: number
  height: number
  stride: number
}

export class FrameRenderer {
  private gl: WebGL2RenderingContext
  private program: WebGLProgram
  private vao: WebGLVertexArrayObject
  private texture: WebGLTexture
  private texW = 0
  private texH = 0

  /**
   * 纹理里是不是已经装着一帧像素。
   *
   * 服务端会标出"这一帧和上一帧像素完全相同"，渲染侧据此跳过上传（静止画面
   * 一秒能省下几百 MB 的 GPU 上传）。但前提是**纹理里确实有东西** —— 刚建好
   * 还没传过任何一帧时，跳过上传就只剩一块黑布。
   */
  get hasFrame(): boolean {
    return this.texW > 0 && this.texH > 0
  }
  private disposed = false

  // uniform 的位置只在程序链接后变一次，**缓存起来别每帧查**。
  //
  // getUniformLocation 是一次同步的 JS↔GL 往返，浏览器还要按名字做一次符号
  // 查找并建 WebGLUniformLocation 对象。144Hz 预览下每帧查两次（u_tex、u_scale）
  // 就是 288 次/秒的同步往返，全部压在渲染主线程上 —— 而主线程同时还要做
  // 共享内存 ArrayBuffer 的纹理上传，这里省下的每一次往返都直接换算成帧时间。
  private uTexLoc: WebGLUniformLocation | null = null
  private uScaleLoc: WebGLUniformLocation | null = null

  /** 最近一次上传的几何信息，用于计算显示比例 */
  public sourceWidth = 0
  public sourceHeight = 0
  /** 是否因为超出 GPU 纹理上限而失败 */
  public lastError: string | null = null

  constructor(private canvas: HTMLCanvasElement) {
    const gl = canvas.getContext('webgl2', {
      alpha: false,
      antialias: false,
      depth: false,
      stencil: false,
      desynchronized: true,
      preserveDrawingBuffer: false,
      powerPreference: 'high-performance',
    })
    if (!gl) throw new Error('这个环境不支持 WebGL2')
    this.gl = gl

    const vs = compile(gl, gl.VERTEX_SHADER, VERTEX_SRC)
    const fs = compile(gl, gl.FRAGMENT_SHADER, FRAGMENT_SRC)
    const program = gl.createProgram()
    if (!program) throw new Error('createProgram failed')
    gl.attachShader(program, vs)
    gl.attachShader(program, fs)
    gl.linkProgram(program)
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      throw new Error(`program link failed: ${gl.getProgramInfoLog(program)}`)
    }
    gl.deleteShader(vs)
    gl.deleteShader(fs)
    this.program = program

    // 全屏四边形（两个三角形），UV 用 0..1
    const vao = gl.createVertexArray()
    const vbo = gl.createBuffer()
    if (!vao || !vbo) throw new Error('VAO/VBO 创建失败')
    this.vao = vao
    gl.bindVertexArray(vao)
    gl.bindBuffer(gl.ARRAY_BUFFER, vbo)
    gl.bufferData(
      gl.ARRAY_BUFFER,
      new Float32Array([0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1]),
      gl.STATIC_DRAW,
    )
    const loc = gl.getAttribLocation(program, 'a_pos')
    gl.enableVertexAttribArray(loc)
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0)
    gl.bindVertexArray(null)

    // uniform 位置一次查好（见字段声明处的注释）。
    this.uTexLoc = gl.getUniformLocation(program, 'u_tex')
    this.uScaleLoc = gl.getUniformLocation(program, 'u_scale')

    const tex = gl.createTexture()
    if (!tex) throw new Error('createTexture failed')
    this.texture = tex
    gl.bindTexture(gl.TEXTURE_2D, tex)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1)
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false)
  }

  /** 上传一帧像素。pixels 必须是 BGRA8 的紧凑视图 */
  upload(pixels: Uint8Array, geom: FrameGeometry): boolean {
    if (this.disposed) return false
    const gl = this.gl
    const { width, height, stride } = geom
    if (!width || !height) return false

    const maxTex = gl.getParameter(gl.MAX_TEXTURE_SIZE) as number
    if (width > maxTex || height > maxTex) {
      this.lastError = `画面 ${width}×${height} 超过 GPU 纹理上限 ${maxTex}`
      return false
    }

    gl.bindTexture(gl.TEXTURE_2D, this.texture)

    // WebGL2 的 UNPACK_ROW_LENGTH 单位是"像素"，正好对应后端的 stride/4
    const rowLengthPx = Math.floor(stride / 4)
    const resized = width !== this.texW || height !== this.texH
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, rowLengthPx)

    if (resized) {
      // 重新分配纹理存储，之后每帧只做 texSubImage2D
      gl.texImage2D(
        gl.TEXTURE_2D,
        0,
        gl.RGBA8,
        width,
        height,
        0,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        pixels,
      )
      this.texW = width
      this.texH = height
      this.sourceWidth = width
      this.sourceHeight = height
    } else {
      gl.texSubImage2D(
        gl.TEXTURE_2D,
        0,
        0,
        0,
        width,
        height,
        gl.RGBA,
        gl.UNSIGNED_BYTE,
        pixels,
      )
    }
    gl.pixelStorei(gl.UNPACK_ROW_LENGTH, 0)
    this.lastError = null
    return true
  }

  /** 把纹理按"保持宽高比、居中留黑边"的方式画满 canvas */
  draw(): void {
    if (this.disposed) return
    const gl = this.gl
    const dpr = Math.min(window.devicePixelRatio || 1, 2)
    const cssW = this.canvas.clientWidth || 1
    const cssH = this.canvas.clientHeight || 1
    let w = Math.max(1, Math.round(cssW * dpr))
    let h = Math.max(1, Math.round(cssH * dpr))
    // 画布不许比源更细。
    //
    // 源只有 1280x720（服务侧已经给预览降过采样），而高分屏（dpr 2）上一块
    // 1200px 宽的面板会开出 2400x1350 的画布 —— 那是源的 3.5 倍像素，每帧
    // 多填三百多万像素，画面却不会更清楚（放大采样出来的，细节本来就没有）。
    // 把画布压回源的宽度，剩下的放大交给浏览器合成画布那一步（几乎免费）。
    if (this.sourceWidth > 0 && w > this.sourceWidth) {
      const k = this.sourceWidth / w
      w = Math.max(1, Math.round(w * k))
      h = Math.max(1, Math.round(h * k))
    }
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w
      this.canvas.height = h
    }

    gl.viewport(0, 0, w, h)
    gl.clearColor(0.02, 0.03, 0.05, 1)
    gl.clear(gl.COLOR_BUFFER_BIT)

    gl.useProgram(this.program)
    gl.bindVertexArray(this.vao)
    gl.activeTexture(gl.TEXTURE0)
    gl.bindTexture(gl.TEXTURE_2D, this.texture)
    gl.uniform1i(this.uTexLoc, 0)
    // 保持宽高比：取两个方向缩放的较小者，画面就完整落在视口内，
    // 多出来的那一对轴不铺满、露出黑边（letterbox），而不是被拉伸变形。
    // 这是需求 (3) 明确要求的 —— 多路源宽高比不同（整屏 16:9、窗口 1.92:1）
    // 时必须各按各的比例，否则至少有一路是歪的。
    const sx = this.sourceWidth > 0 ? w / this.sourceWidth : 1
    const sy = this.sourceHeight > 0 ? h / this.sourceHeight : 1
    const fit = Math.min(sx, sy)
    const scaleX = this.sourceWidth > 0 ? (this.sourceWidth * fit) / w : 1
    const scaleY = this.sourceHeight > 0 ? (this.sourceHeight * fit) / h : 1
    gl.uniform2f(this.uScaleLoc, scaleX, scaleY)
    gl.drawArrays(gl.TRIANGLES, 0, 6)
    gl.bindVertexArray(null)
  }

  /**
   * 把画布清成底色（不画任何画面）。
   *
   * 为什么必须有这个方法：WebGL 的 canvas **不会自己清空**，`preserveDrawingBuffer`
   * 关着也只是"下一帧绘制前才清"，只要不再调用 draw()，上一帧就一直留在那张
   * 缓冲区里。切到另一路预览时如果只把 view 置空、不再绘制，用户看到的就是
   * **上一路的残影**（踩过：切到匹配不到窗口的那一路，NO SIGNAL 一消失，
   * 底下露出的是显示器捕获那张整屏画面，得切回去再切回来才刷新）。
   *
   * 尺寸也要跟着 canvas 的 CSS 尺寸重设一次：切路后浏览器可能已经改了布局，
   * 沿用旧视口会让清屏只覆盖一部分。
   */
  clear(): void {
    if (this.disposed) return
    const gl = this.gl
    const dpr = Math.min(window.devicePixelRatio || 1, 2)
    const cssW = this.canvas.clientWidth || 1
    const cssH = this.canvas.clientHeight || 1
    const w = Math.max(1, Math.round(cssW * dpr))
    const h = Math.max(1, Math.round(cssH * dpr))
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w
      this.canvas.height = h
    }
    gl.viewport(0, 0, w, h)
    gl.clearColor(0.02, 0.03, 0.05, 1)
    gl.clear(gl.COLOR_BUFFER_BIT)
  }

  /**
   * 当前是否处于「留黑边」状态（供自检核对比例确实生效）。
   * 返回画布内实际画面的尺寸，便于和源宽高比比对。
   */
  letterboxRect(cssW: number, cssH: number): { width: number; height: number } {
    if (this.sourceWidth <= 0 || this.sourceHeight <= 0) {
      return { width: cssW, height: cssH }
    }
    const fit = Math.min(cssW / this.sourceWidth, cssH / this.sourceHeight)
    return { width: this.sourceWidth * fit, height: this.sourceHeight * fit }
  }

  dispose(): void {
    if (this.disposed) return
    this.disposed = true
    const gl = this.gl
    gl.deleteTexture(this.texture)
    gl.deleteProgram(this.program)
    gl.deleteVertexArray(this.vao)
  }
}
