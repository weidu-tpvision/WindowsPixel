package com.winpixel.app

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.browser.customtabs.CustomTabsIntent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

class MainActivity : ComponentActivity() {

    private var surfaceViewRef: SurfaceView? = null
    private var trackpadMode by mutableStateOf(false)
    private var guestWidth = 800
    private var guestHeight = 600
    private var cursorX = 0.5
    private var cursorY = 0.5
    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var downTimeMs = 0L

    private val requestMicPerm = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { /* AAudio handles missing permission gracefully */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
            != PackageManager.PERMISSION_GRANTED
        ) {
            requestMicPerm.launch(Manifest.permission.RECORD_AUDIO)
        }

        val machineDir = File(getExternalFilesDir(null), "Machines/Win11").apply { mkdirs() }
        val shareDir = File(machineDir, "Shared").apply { mkdirs() }
        val shareSock = File(cacheDir, "winvm_share.sock")

        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                WinPixelScreen(
                    machineDir = machineDir,
                    shareDir = shareDir,
                    shareSock = shareSock
                )
            }
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    private fun hideSystemBars() {
        val controller = WindowInsetsControllerCompat(window, window.decorView)
        controller.hide(WindowInsetsCompat.Type.systemBars())
        controller.systemBarsBehavior =
            WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
    }

    @Composable
    private fun WinPixelScreen(machineDir: File, shareDir: File, shareSock: File) {
        val scope = rememberCoroutineScope()
        var running by remember { mutableStateOf(WinVmBridge.nativeIsRunning()) }
        var starting by remember { mutableStateOf(false) }
        var errorMsg by remember { mutableStateOf<String?>(null) }
        var cpus by remember { mutableIntStateOf(4) }
        var ramMb by remember { mutableIntStateOf(4096) }
        var soundEnabled by remember { mutableStateOf(true) }
        var showToolbar by remember { mutableStateOf(true) }

        val codeFile = remember(machineDir) { File(machineDir, "code.fd") }
        val varsFile = remember(machineDir) { File(machineDir, "vars.fd") }
        val diskFile = remember(machineDir) {
            listOf("windows11.hvsparse", "windows11.img")
                .map { File(machineDir, it) }
                .firstOrNull { it.exists() } ?: File(machineDir, "windows11.img")
        }
        val disk1File = remember(machineDir) {
            listOf("virtio_net.img", "installer.img")
                .map { File(machineDir, it) }
                .firstOrNull { it.exists() }
        }

        val ready = codeFile.exists() && varsFile.exists() && diskFile.exists()

        LaunchedEffect(running) {
            while (running) {
                if (!WinVmBridge.nativeIsRunning()) {
                    running = false
                    VmForegroundService.stop(this@MainActivity)
                    break
                }
                val sz = WinVmBridge.nativeDisplaySize()
                if (sz.size >= 2 && sz[0] > 0 && sz[1] > 0) {
                    guestWidth = sz[0]
                    guestHeight = sz[1]
                }
                val oauthUrl = WinVmBridge.nativePollOpenUrl()
                if (!oauthUrl.isNullOrBlank()) {
                    runCatching {
                        CustomTabsIntent.Builder().build()
                            .launchUrl(this@MainActivity, Uri.parse(oauthUrl))
                    }
                }
                delay(250)
            }
        }

        Box(modifier = Modifier.fillMaxSize().background(Color.Black)) {
            if (running) {
                AndroidView(
                    modifier = Modifier.fillMaxSize(),
                    factory = { ctx ->
                        VmSurfaceView(ctx).apply {
                            isFocusable = true
                            isFocusableInTouchMode = true
                            surfaceViewRef = this
                            holder.addCallback(object : SurfaceHolder.Callback {
                                override fun surfaceCreated(holder: SurfaceHolder) {
                                    WinVmBridge.nativeSetSurface(holder.surface)
                                }

                                override fun surfaceChanged(
                                    holder: SurfaceHolder,
                                    format: Int,
                                    width: Int,
                                    height: Int
                                ) {
                                    WinVmBridge.nativeSetSurface(holder.surface)
                                }

                                override fun surfaceDestroyed(holder: SurfaceHolder) {
                                    WinVmBridge.nativeSetSurface(null)
                                }
                            })
                            setOnTouchListener { v, ev ->
                                handleSurfaceTouch(v, ev)
                                true
                            }
                        }
                    }
                )

                if (showToolbar) {
                    Row(
                        modifier = Modifier
                            .align(Alignment.TopCenter)
                            .padding(top = 10.dp)
                            .background(Color(0xBB141820), RoundedCornerShape(24.dp))
                            .padding(horizontal = 12.dp, vertical = 6.dp),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        TextButton(onClick = { trackpadMode = !trackpadMode }) {
                            Text(if (trackpadMode) "Trackpad" else "Touch", color = Color.White, fontSize = 12.sp)
                        }
                        TextButton(onClick = { toggleSoftKeyboard() }) {
                            Text("Keyboard", color = Color.White, fontSize = 12.sp)
                        }
                        TextButton(onClick = { WinVmBridge.nativeSendCtrlAltDel() }) {
                            Text("Ctrl+Alt+Del", color = Color.White, fontSize = 12.sp)
                        }
                        TextButton(onClick = { WinVmBridge.nativePowerButton() }) {
                            Text("Power", color = Color(0xFFFFB74D), fontSize = 12.sp)
                        }
                        TextButton(onClick = {
                            WinVmBridge.nativeStopVm()
                            VmForegroundService.stop(this@MainActivity)
                            running = false
                        }) {
                            Text("Stop", color = Color(0xFFEF5350), fontSize = 12.sp)
                        }
                        TextButton(onClick = { showToolbar = false }) {
                            Text("Hide", color = Color.Gray, fontSize = 12.sp)
                        }
                    }
                } else {
                    TextButton(
                        modifier = Modifier
                            .align(Alignment.TopEnd)
                            .padding(8.dp)
                            .background(Color(0x66141820), RoundedCornerShape(16.dp)),
                        onClick = { showToolbar = true }
                    ) {
                        Text("Menu", color = Color.White, fontSize = 11.sp)
                    }
                }
            } else {
                Column(
                    modifier = Modifier
                        .align(Alignment.Center)
                        .widthIn(max = 620.dp)
                        .padding(24.dp)
                        .background(Color(0xFF151922), RoundedCornerShape(16.dp))
                        .padding(24.dp),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Text("WindowsPixel — Pixel 9 / ARM64", style = MaterialTheme.typography.headlineSmall, color = Color.White)
                    Text(
                        "Machine folder:\n${machineDir.absolutePath}",
                        fontFamily = FontFamily.Monospace,
                        fontSize = 12.sp,
                        color = Color(0xFF90CAF9)
                    )
                    Text(
                        "• code.fd: ${if (codeFile.exists()) "OK" else "Missing"}\n" +
                            "• vars.fd: ${if (varsFile.exists()) "OK" else "Missing"}\n" +
                            "• ${diskFile.name}: ${if (diskFile.exists()) "OK" else "Missing"}\n" +
                            "• disk1: ${disk1File?.name ?: "None"}",
                        fontFamily = FontFamily.Monospace,
                        fontSize = 13.sp,
                        color = Color(0xFFE0E0E0)
                    )
                    if (!ready) {
                        Text(
                            "Push files via ADB:\nadb push work/code.fd work/vars.fd work/windows11.img ${machineDir.absolutePath}/",
                            fontFamily = FontFamily.Monospace,
                            fontSize = 11.sp,
                            color = Color(0xFFFFCC80)
                        )
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                        Text("vCPUs: $cpus", color = Color.White, fontSize = 13.sp)
                        Slider(
                            value = cpus.toFloat(),
                            onValueChange = { cpus = it.toInt() },
                            valueRange = 1f..4f,
                            steps = 2,
                            modifier = Modifier.weight(1f)
                        )
                        Text("RAM: ${ramMb}MB", color = Color.White, fontSize = 13.sp)
                        Slider(
                            value = ramMb.toFloat(),
                            onValueChange = { ramMb = (it.toInt() / 512) * 512 },
                            valueRange = 2048f..6144f,
                            steps = 7,
                            modifier = Modifier.weight(1f)
                        )
                    }
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = soundEnabled, onCheckedChange = { soundEnabled = it })
                        Text("Enable AAudio Duplex Sound", color = Color.White, fontSize = 13.sp)
                    }
                    if (errorMsg != null) {
                        Text(errorMsg!!, color = Color(0xFFEF5350), fontSize = 13.sp)
                    }
                    Button(
                        enabled = ready && !starting,
                        onClick = {
                            errorMsg = null
                            starting = true
                            VmForegroundService.start(this@MainActivity)
                            scope.launch {
                                val err = withContext(Dispatchers.IO) {
                                    WinVmBridge.nativeStartVm(
                                        codeFile.absolutePath,
                                        varsFile.absolutePath,
                                        diskFile.absolutePath,
                                        disk1File?.absolutePath,
                                        shareDir.absolutePath,
                                        shareSock.absolutePath,
                                        cpus,
                                        ramMb,
                                        soundEnabled,
                                        false
                                    )
                                }
                                starting = false
                                if (err != null) {
                                    errorMsg = err
                                    VmForegroundService.stop(this@MainActivity)
                                } else {
                                    running = true
                                }
                            }
                        },
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Text(if (starting) "Starting VM..." else "Boot Windows 11 ARM64")
                    }
                }
            }
        }
    }

    private fun toggleSoftKeyboard() {
        val v = surfaceViewRef ?: window.decorView
        v.requestFocus()
        val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.showSoftInput(v, InputMethodManager.SHOW_IMPLICIT)
    }

    private fun mapTouchToGuest(view: View, rawX: Float, rawY: Float): Pair<Double, Double> {
        val vw = view.width.toDouble().coerceAtLeast(1.0)
        val vh = view.height.toDouble().coerceAtLeast(1.0)
        val gw = guestWidth.toDouble().coerceAtLeast(1.0)
        val gh = guestHeight.toDouble().coerceAtLeast(1.0)
        val guestAspect = gw / gh
        val viewAspect = vw / vh
        val rx: Double
        val ry: Double
        val rw: Double
        val rh: Double
        if (viewAspect > guestAspect) {
            rh = vh
            rw = vh * guestAspect
            rx = (vw - rw) * 0.5
            ry = 0.0
        } else {
            rw = vw
            rh = vw / guestAspect
            rx = 0.0
            ry = (vh - rh) * 0.5
        }
        val nx = ((rawX.toDouble() - rx) / rw).coerceIn(0.0, 1.0)
        val ny = ((rawY.toDouble() - ry) / rh).coerceIn(0.0, 1.0)
        return nx to ny
    }

    private fun handleSurfaceTouch(view: View, ev: MotionEvent) {
        if (!trackpadMode) {
            val (nx, ny) = mapTouchToGuest(view, ev.x, ev.y)
            cursorX = nx
            cursorY = ny
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> WinVmBridge.nativeTabletEvent(nx, ny, 1, 0)
                MotionEvent.ACTION_MOVE -> WinVmBridge.nativeTabletEvent(nx, ny, 1, 0)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> WinVmBridge.nativeTabletEvent(nx, ny, 0, 0)
                MotionEvent.ACTION_POINTER_DOWN -> if (ev.pointerCount == 2) WinVmBridge.nativeTabletEvent(nx, ny, 2, 0)
                MotionEvent.ACTION_POINTER_UP -> WinVmBridge.nativeTabletEvent(nx, ny, 0, 0)
            }
            return
        }

        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                lastTouchX = ev.x
                lastTouchY = ev.y
                downTimeMs = ev.eventTime
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = (ev.x - lastTouchX) / view.width.coerceAtLeast(1)
                val dy = (ev.y - lastTouchY) / view.height.coerceAtLeast(1)
                lastTouchX = ev.x
                lastTouchY = ev.y
                cursorX = (cursorX + dx * 1.4).coerceIn(0.0, 1.0)
                cursorY = (cursorY + dy * 1.4).coerceIn(0.0, 1.0)
                WinVmBridge.nativeTabletEvent(cursorX, cursorY, 0, 0)
            }
            MotionEvent.ACTION_UP -> {
                if (ev.eventTime - downTimeMs < 220) {
                    WinVmBridge.nativeTabletEvent(cursorX, cursorY, 1, 0)
                    WinVmBridge.nativeTabletEvent(cursorX, cursorY, 0, 0)
                }
            }
        }
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (WinVmBridge.nativeIsRunning()) {
            val down = event.action == KeyEvent.ACTION_DOWN
            val up = event.action == KeyEvent.ACTION_UP
            if (down || up) {
                val usage = androidKeyToHid(event.keyCode)
                if (usage != 0) {
                    if (event.isShiftPressed && down) WinVmBridge.nativeKeyHid(0xe1, true)
                    WinVmBridge.nativeKeyHid(usage, down)
                    if (event.isShiftPressed && up) WinVmBridge.nativeKeyHid(0xe1, false)
                    return true
                }
            }
        }
        return super.dispatchKeyEvent(event)
    }

    private fun androidKeyToHid(keyCode: Int): Int {
        if (keyCode in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z) {
            return 0x04 + (keyCode - KeyEvent.KEYCODE_A)
        }
        if (keyCode in KeyEvent.KEYCODE_1..KeyEvent.KEYCODE_9) {
            return 0x1e + (keyCode - KeyEvent.KEYCODE_1)
        }
        return when (keyCode) {
            KeyEvent.KEYCODE_0 -> 0x27
            KeyEvent.KEYCODE_ENTER -> 0x28
            KeyEvent.KEYCODE_ESCAPE -> 0x29
            KeyEvent.KEYCODE_DEL -> 0x2a
            KeyEvent.KEYCODE_TAB -> 0x2b
            KeyEvent.KEYCODE_SPACE -> 0x2c
            KeyEvent.KEYCODE_MINUS -> 0x2d
            KeyEvent.KEYCODE_EQUALS -> 0x2e
            KeyEvent.KEYCODE_LEFT_BRACKET -> 0x2f
            KeyEvent.KEYCODE_RIGHT_BRACKET -> 0x30
            KeyEvent.KEYCODE_BACKSLASH -> 0x31
            KeyEvent.KEYCODE_SEMICOLON -> 0x33
            KeyEvent.KEYCODE_APOSTROPHE -> 0x34
            KeyEvent.KEYCODE_GRAVE -> 0x35
            KeyEvent.KEYCODE_COMMA -> 0x36
            KeyEvent.KEYCODE_PERIOD -> 0x37
            KeyEvent.KEYCODE_SLASH -> 0x38
            KeyEvent.KEYCODE_DPAD_RIGHT -> 0x4f
            KeyEvent.KEYCODE_DPAD_LEFT -> 0x50
            KeyEvent.KEYCODE_DPAD_DOWN -> 0x51
            KeyEvent.KEYCODE_DPAD_UP -> 0x52
            else -> 0
        }
    }

    private fun typeAsciiChar(ch: Char) {
        var shift = false
        val usage = when (ch) {
            in 'a'..'z' -> 0x04 + (ch - 'a')
            in 'A'..'Z' -> { shift = true; 0x04 + (ch - 'A') }
            in '1'..'9' -> 0x1e + (ch - '1')
            '0' -> 0x27
            ' ' -> 0x2c
            '\n', '\r' -> 0x28
            '\t' -> 0x2b
            '-' -> 0x2d
            '_' -> { shift = true; 0x2d }
            '=' -> 0x2e
            '+' -> { shift = true; 0x2e }
            '[' -> 0x2f
            '{' -> { shift = true; 0x2f }
            ']' -> 0x30
            '}' -> { shift = true; 0x30 }
            '\\' -> 0x31
            '|' -> { shift = true; 0x31 }
            ';' -> 0x33
            ':' -> { shift = true; 0x33 }
            '\'' -> 0x34
            '"' -> { shift = true; 0x34 }
            '`' -> 0x35
            '~' -> { shift = true; 0x35 }
            ',' -> 0x36
            '<' -> { shift = true; 0x36 }
            '.' -> 0x37
            '>' -> { shift = true; 0x37 }
            '/' -> 0x38
            '?' -> { shift = true; 0x38 }
            '!' -> { shift = true; 0x1e }
            '@' -> { shift = true; 0x1f }
            '#' -> { shift = true; 0x20 }
            '$' -> { shift = true; 0x21 }
            '%' -> { shift = true; 0x22 }
            '^' -> { shift = true; 0x23 }
            '&' -> { shift = true; 0x24 }
            '*' -> { shift = true; 0x25 }
            '(' -> { shift = true; 0x26 }
            ')' -> { shift = true; 0x27 }
            else -> 0
        }
        if (usage == 0) return
        if (shift) WinVmBridge.nativeKeyHid(0xe1, true)
        WinVmBridge.nativeKeyHid(usage, true)
        WinVmBridge.nativeKeyHid(usage, false)
        if (shift) WinVmBridge.nativeKeyHid(0xe1, false)
    }

    private inner class VmSurfaceView(context: Context) : SurfaceView(context) {
        override fun onCheckIsTextEditor(): Boolean = true

        override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
            outAttrs.inputType = EditorInfo.TYPE_CLASS_TEXT or EditorInfo.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_ACTION_NONE
            return object : BaseInputConnection(this, false) {
                override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                    text?.forEach { typeAsciiChar(it) }
                    return true
                }

                override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                    repeat(beforeLength.coerceAtLeast(0)) {
                        WinVmBridge.nativeKeyHid(0x2a, true)
                        WinVmBridge.nativeKeyHid(0x2a, false)
                    }
                    return true
                }
            }
        }
    }
}
