package org.huxerui.lib.archive.preview;

import android.app.Application;

public final class MainApplication extends Application {
    static {
        System.loadLibrary(BuildConfig.HUXERUI_APP_LIBRARY);
    }
}
