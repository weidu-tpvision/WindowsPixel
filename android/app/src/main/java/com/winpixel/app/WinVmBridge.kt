package com.winpixel.app

import android.view.Surface

object WinVmBridge {
    init {
        System.loadLibrary("winvm_jni")
    }

    external fun nativeStartVm(
        codeFd: String,
        varsFd: String,
        disk0: String,
        disk1: String?,
        shareDir: String?,
        shareSock: String?,
        cpus: Int,
        ramMb: Int,
        sound: Boolean,
        noNet: Boolean
    ): String?

    external fun nativeStopVm()
    external fun nativePowerButton()
    external fun nativeSetSurface(surface: Surface?)
    external fun nativeTabletEvent(xNorm: Double, yNorm: Double, buttons: Int, wheel: Int)
    external fun nativeKeyHid(usage: Int, down: Boolean)
    external fun nativeKeyLinux(code: Int, down: Boolean)
    external fun nativeSendCtrlAltDel()
    external fun nativeIsRunning(): Boolean
    external fun nativeDisplaySize(): IntArray
    external fun nativePollOpenUrl(): String?
}
