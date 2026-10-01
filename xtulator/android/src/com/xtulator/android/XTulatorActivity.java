package com.xtulator.android;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.graphics.Paint;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public class XTulatorActivity extends Activity {

    private static final String NATIVE_LIB = "xtulator";
    private static final int SAMPLE_RATE = 48000;
    private static final int BUFFER_SIZE = 4096;

    public native int nativeInit(String biosPath, String diskPath);
    public native void nativeRun();
    public native void nativeStop();
    public native void nativeKeyDown(int keyCode);
    public native void nativeKeyUp(int keyCode);

    /**
     * Must match XT_ENABLE_AUDIO in XTulator/config.h. When false we skip creating
     * the AudioTrack and its flush thread entirely so they do not compete with the
     * emulator for the single CPU core.
     */
    private static final boolean NATIVE_AUDIO_ENABLED = false;
    public native void nativeMouseEvent(int action, int x, int y);
    public native void nativeResize(int width, int height);
    public native void nativePause();
    public native void nativeResume();
    public native int nativeGetFbWidth();
    public native int nativeGetFbHeight();
    public native void nativeGetFbDims(int[] dims);
    public native int nativeGetFrame(int[] dims, int[] pixels);
    public native int nativeGetSpeedPct();
    public native int nativeGetDynrecNativeBlocks();
    public native int nativeGetDynrecInterpreterInstrs();
    public native boolean nativeCopyAsset(android.content.res.AssetManager am, String assetName, String outPath);
    public native int nativeCopyFrame(int[] out);
    public native void nativeFlushAudio();
    public native void nativeReset();
    public native boolean nativeChangeFloppy(String path);

    /** Trace flag bitmask values — must match debuglog.h TRACE_FLAG_* defines. */
    private static final int TRACE_CPU      = 0x01;
    private static final int TRACE_KEYBOARD = 0x02;
    private static final int TRACE_INTERRUPTS = 0x10;
    private static final int TRACE_DISK     = 0x04;
    private static final int TRACE_MISC     = 0x40;
    private static final int TRACE_VIDEO    = 0x20;
    /** JNI call to toggle a single trace category bit in the native core. */
    public native void nativeSetTraceFlag(int flag, boolean enabled);
    public native void nativeEnableDynrec(boolean enable);

    private Thread mEmulatorThread = null;
    private XTulatorView mEmulatorView = null;
    private boolean mShowSpeed = false;
    private boolean mTraceCpu = false;
    private boolean mTraceKeyboard = false;
    private boolean mTraceInterrupts = false;
    private boolean mTraceDisk = false;
    private boolean mTraceMisc = false;
    private boolean         mDynrecEnabled = false;
    private AudioTrack mAudioTrack = null;
    private Thread mAudioFlushThread = null;
    private volatile boolean mAudioRunning = false;

    static {
        System.loadLibrary(NATIVE_LIB);
        Log.i("XTulator", "Native library loaded: " + NATIVE_LIB);
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(
            WindowManager.LayoutParams.FLAG_FULLSCREEN,
            WindowManager.LayoutParams.FLAG_FULLSCREEN
        );
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        mEmulatorView = new XTulatorView(this);
        setContentView(mEmulatorView);

        copyAssetsRecursive();

        // Copy fd.img to disk.img if fd.img exists and disk.img doesn't (or is stub)
        File filesDir = getFilesDir();
        File fdImg = new File(filesDir, "fd.img");
        File diskImg = new File(filesDir, "disk.img");
        if (fdImg.exists() && (!diskImg.exists() || diskImg.length() < fdImg.length())) {
            try {
                java.io.FileInputStream in = new java.io.FileInputStream(fdImg);
                java.io.FileOutputStream out = new java.io.FileOutputStream(diskImg);
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
                in.close();
                out.close();
                Log.i("XTulator", "Copied fd.img to disk.img: " + diskImg.getAbsolutePath());
            } catch (IOException e) {
                Log.e("XTulator", "Failed to copy fd.img to disk.img: " + e.getMessage());
            }
        }

        // Use app files directory for BIOS and disk
        String biosPath = new File(filesDir, "bios128k-2.0.bin").getAbsolutePath();
        String diskPath = diskImg.getAbsolutePath();

        // The native code chdirs to the directory containing biosPath, then loads
        // ROMs relative to that directory. Since assets are copied to filesDir/roms/...,
        // we pass a biosPath in filesDir root so chdir goes to filesDir/.
        // The 128KB BIOS is already at filesDir/roms/machine/xi8088/bios128k-2.0.bin from asset copy.

        // Copy VGA BIOS (ET4000) to expected location
        File vgaRomDir = new File(filesDir, "roms/video");
        if (!vgaRomDir.exists()) {
            vgaRomDir.mkdirs();
        }
        File vgaRomBios = new File(vgaRomDir, "et4000.bin");
        if (!vgaRomBios.exists()) {
            try {
                java.io.FileInputStream in = new java.io.FileInputStream(new File(filesDir, "roms/video/et4000.bin").getAbsolutePath());
                java.io.FileOutputStream out = new java.io.FileOutputStream(vgaRomBios);
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
                in.close();
                out.close();
                Log.i("XTulator", "Copied VGA BIOS to expected location: " + vgaRomBios.getAbsolutePath());
            } catch (IOException e) {
                Log.e("XTulator", "Failed to copy VGA BIOS: " + e.getMessage());
            }
        }

        int result = nativeInit(biosPath, diskPath);
        if (result != 0) {
            Log.e("XTulator", "Native init failed: " + result);
        }

        restoreTraceSettings();

        initAudioTrack();

        Log.i("XTulator", "Activity created");
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        MenuInflater inflater = getMenuInflater();
        inflater.inflate(R.menu.menu, menu);
        return true;
    }

    @Override
    public boolean onMenuItemSelected(int featureId, MenuItem item) {
        if (item.getItemId() == R.id.menu_dynrec) {
            Log.i("XTulator", "Menu: Dynrec toggle selected");
            mDynrecEnabled = !mDynrecEnabled;
            item.setChecked(mDynrecEnabled);
            nativeEnableDynrec(mDynrecEnabled);
            saveTraceSettings();
            return true;
        } else if (item.getItemId() == R.id.menu_reset) {
            Log.i("XTulator", "Menu: Reset selected");
            nativeReset();
            return true;
        } else if (item.getItemId() == R.id.menu_changedisk) {
            Log.i("XTulator", "Menu: Change Floppy selected");
            Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
            intent.setType("*/*");
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(Intent.createChooser(intent, "Select Floppy Image"), 1001);
            return true;
        } else if (item.getItemId() == R.id.menu_showspeed) {
            mShowSpeed = !mShowSpeed;
            item.setChecked(mShowSpeed);
            saveTraceSettings();
            return true;
        } else if (item.getItemId() == R.id.menu_trace_cpu) {
            mTraceCpu = !mTraceCpu;
            item.setChecked(mTraceCpu);
            nativeSetTraceFlag(TRACE_CPU, mTraceCpu);
            saveTraceSettings();
            return true;
        } else if (item.getItemId() == R.id.menu_trace_keyboard) {
            mTraceKeyboard = !mTraceKeyboard;
            item.setChecked(mTraceKeyboard);
            nativeSetTraceFlag(TRACE_KEYBOARD, mTraceKeyboard);
            saveTraceSettings();
            return true;
        } else if (item.getItemId() == R.id.menu_trace_interrupts) {
            mTraceInterrupts = !mTraceInterrupts;
            item.setChecked(mTraceInterrupts);
            nativeSetTraceFlag(TRACE_INTERRUPTS, mTraceInterrupts);
            saveTraceSettings();
            return true;
        } else if (item.getItemId() == R.id.menu_trace_disk) {
            mTraceDisk = !mTraceDisk;
            item.setChecked(mTraceDisk);
            nativeSetTraceFlag(TRACE_DISK, mTraceDisk);
            saveTraceSettings();
            return true;
        } else if (item.getItemId() == R.id.menu_trace_misc) {
            mTraceMisc = !mTraceMisc;
            item.setChecked(mTraceMisc);
            nativeSetTraceFlag(TRACE_MISC, mTraceMisc);
            saveTraceSettings();
            return true;
        }
        return super.onMenuItemSelected(featureId, item);
    }

    private void restoreTraceSettings() {
        SharedPreferences prefs = getSharedPreferences("xtulator_prefs", MODE_PRIVATE);
        mShowSpeed = prefs.getBoolean("show_speed", false);
        mTraceCpu = prefs.getBoolean("trace_cpu", false);
        mTraceKeyboard = prefs.getBoolean("trace_keyboard", false);
        mTraceInterrupts = prefs.getBoolean("trace_interrupts", false);
        mTraceDisk = prefs.getBoolean("trace_disk", false);
        mTraceMisc = true; /* Enable by default for debugging */
        nativeSetTraceFlag(TRACE_MISC, true);
        nativeSetTraceFlag(0x80, false); /* TRACE_FLAG_DIAG — disabled for performance */
        nativeSetTraceFlag(TRACE_VIDEO, false); /* Disable video trace to avoid logflood */
        mDynrecEnabled = prefs.getBoolean("dynrec", false);
        nativeEnableDynrec(mDynrecEnabled); /* Dynrec disabled by default — enable via menu */
        Log.i("XTulator", "Restored trace settings: cpu=" + mTraceCpu + " keyboard=" + mTraceKeyboard
              + " interrupts=" + mTraceInterrupts + " disk=" + mTraceDisk + " misc=" + mTraceMisc
              + " showSpeed=" + mShowSpeed);
    }

    private void saveTraceSettings() {
        SharedPreferences prefs = getSharedPreferences("xtulator_prefs", MODE_PRIVATE);
        SharedPreferences.Editor editor = prefs.edit();
        editor.putBoolean("show_speed", mShowSpeed);
        editor.putBoolean("trace_cpu", mTraceCpu);
        editor.putBoolean("trace_keyboard", mTraceKeyboard);
        editor.putBoolean("trace_interrupts", mTraceInterrupts);
        editor.putBoolean("trace_disk", mTraceDisk);
        editor.putBoolean("trace_misc", mTraceMisc);
        editor.putBoolean("dynrec", mDynrecEnabled);
        editor.commit();
    }

    public boolean isShowSpeed() { return mShowSpeed; }
    public boolean isDynrecEnabled() { return mDynrecEnabled; }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == 1001 && resultCode == RESULT_OK && data != null) {
            Uri uri = data.getData();
            if (uri != null) {
                String path = uri.getPath();
                Log.i("XTulator", "Selected floppy: " + path);
                boolean ok = nativeChangeFloppy(path);
                Log.i("XTulator", "Floppy change " + (ok ? "succeeded" : "failed"));
            }
        }
    }

    private void initAudioTrack() {
        // Audio is disabled in the native core (XT_ENABLE_AUDIO in XTulator/config.h):
        // the emulator runs on a single 528MHz core and the AudioTrack plus its
        // flush thread take a real share of it for no benefit while emulating.
        // Flip XT_ENABLE_AUDIO back to 1 to re-enable.
        if (!NATIVE_AUDIO_ENABLED) {
            Log.i("XTulator", "Audio disabled in native core, skipping AudioTrack");
            mAudioTrack = null;
            mAudioRunning = false;
            return;
        }

        int minBufferSize = AudioTrack.getMinBufferSize(
            SAMPLE_RATE,
            AudioFormat.CHANNEL_CONFIGURATION_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        );
        int bufferSize = Math.max(minBufferSize, BUFFER_SIZE);

        mAudioTrack = new AudioTrack(
            AudioManager.STREAM_MUSIC,
            SAMPLE_RATE,
            AudioFormat.CHANNEL_CONFIGURATION_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
            bufferSize,
            AudioTrack.MODE_STREAM
        );

        if (mAudioTrack.getState() == AudioTrack.STATE_INITIALIZED) {
            mAudioTrack.play();
            mAudioRunning = true;
            mAudioFlushThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    audioFlushLoop();
                }
            });
            mAudioFlushThread.start();
            Log.i("XTulator", "AudioTrack initialized and playing");
        } else {
            Log.e("XTulator", "AudioTrack initialization failed");
            mAudioTrack = null;
        }
    }

    private void audioFlushLoop() {
        while (mAudioRunning) {
            nativeFlushAudio();
            try {
                Thread.sleep(10);
            } catch (InterruptedException e) {
                break;
            }
        }
    }

    public static int writeAudioSamples(short[] buffer, int offset, int count) {
        if (buffer == null || count <= 0) {
            return 0;
        }
        XTulatorActivity activity = getInstance();
        if (activity != null && activity.mAudioTrack != null) {
            return activity.mAudioTrack.write(buffer, offset, count);
        }
        return 0;
    }

    private static XTulatorActivity sInstance = null;

    private static XTulatorActivity getInstance() {
        return sInstance;
    }

    @Override
    public void onResume() {
        super.onResume();
        sInstance = this;
        nativeResume();
        if (mEmulatorThread == null || !mEmulatorThread.isAlive()) {
            mEmulatorThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    nativeRun();
                }
            });
            mEmulatorThread.start();
        }
        mEmulatorView.startRender();
        if (mAudioTrack != null && mAudioTrack.getPlayState() != AudioTrack.PLAYSTATE_PLAYING) {
            mAudioTrack.play();
        }
        Log.i("XTulator", "Activity resumed");
    }

    @Override
    public void onPause() {
        super.onPause();
        saveTraceSettings();
        sInstance = null;
        mEmulatorView.stopRender();
        nativePause();
        if (mAudioTrack != null) {
            mAudioTrack.pause();
        }
        Log.i("XTulator", "Activity paused");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        sInstance = null;
        nativeStop();
        if (mEmulatorThread != null) {
            try {
                mEmulatorThread.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            mEmulatorThread = null;
        }
        mEmulatorView.stopRender();
        mAudioRunning = false;
        if (mAudioFlushThread != null) {
            try {
                mAudioFlushThread.join(500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            mAudioFlushThread = null;
        }
        if (mAudioTrack != null) {
            mAudioTrack.stop();
            mAudioTrack.release();
            mAudioTrack = null;
        }
        Log.i("XTulator", "Activity destroyed");
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int keyCode = event.getKeyCode();
        // Forward all keys to the emulator, except MENU and BACK which are
        // reserved for the on-screen options menu. (dispatchKeyEvent still
        // receives MENU/BACK so the Activity chain can open the menu.)
        if (keyCode != KeyEvent.KEYCODE_MENU && keyCode != KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                nativeKeyDown(keyCode);
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                nativeKeyUp(keyCode);
            }
            return true; // Consume - sent to emulator
        }
        return super.dispatchKeyEvent(event); // Let Activity handle MENU/BACK
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode != KeyEvent.KEYCODE_MENU && keyCode != KeyEvent.KEYCODE_BACK) {
            nativeKeyDown(keyCode);
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (keyCode != KeyEvent.KEYCODE_MENU && keyCode != KeyEvent.KEYCODE_BACK) {
            nativeKeyUp(keyCode);
        }
        return super.onKeyUp(keyCode, event);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getAction();
        int x = (int) event.getX();
        int y = (int) event.getY();
        nativeMouseEvent(action, x, y);
        return true;
    }

    @Override
    public boolean onTrackballEvent(MotionEvent event) {
        int action = event.getAction();
        int x = (int) event.getX();
        int y = (int) event.getY();
        nativeMouseEvent(action, x, y);
        return true;
    }

    private void copyAssetsRecursive() {
        AssetManager am = getAssets();
        File dir = getFilesDir();
        Log.i("XTulator", "Starting asset copy from assets to " + dir.getAbsolutePath());
        try {
            copyAssetDir(am, "", dir);
            Log.i("XTulator", "Asset copy completed successfully");
        } catch (IOException e) {
            Log.e("XTulator", "Asset copy failed (IOException): " + e.getMessage(), e);
        } catch (Exception e) {
            Log.e("XTulator", "Asset copy failed (Exception): " + e.getMessage(), e);
        }
    }

    private void copyAssetDir(AssetManager am, String assetDir, File outDir) throws IOException {
        String[] names = am.list(assetDir);
        Log.i("XTulator", "Listing assets in '" + assetDir + "': " + (names != null ? names.length : "null"));
        if (names == null) {
            Log.w("XTulator", "Asset directory '" + assetDir + "' is null or empty");
            return;
        }
        outDir.mkdirs();
        for (String name : names) {
            if ("README.txt".equals(name) && assetDir.length() == 0) {
                continue;
            }
            String sub;
            if (assetDir.length() == 0) {
                sub = name;
            } else {
                sub = assetDir + "/" + name;
            }
            String[] subNames = am.list(sub);
            File outFile = new File(outDir, name);
            if (subNames != null && subNames.length > 0) {
                Log.i("XTulator", "Recursing into asset directory: " + sub);
                copyAssetDir(am, sub, outFile);
            } else {
                Log.i("XTulator", "Copying asset file: " + sub + " -> " + outFile.getAbsolutePath());
                try {
                    // Always use native AAssetManager to copy assets - avoids AssetManager.readAsset issues
                    // InputStream.available() returns unreliable values for Android assets
                    Log.i("XTulator", "Using native copy for asset");
                    boolean ok = nativeCopyAsset(am, sub, outFile.getAbsolutePath());
                    if (!ok) {
                        throw new IOException("Native copy failed for " + sub);
                    }
                    Log.i("XTulator", "Copied asset: " + sub);
                } catch (IOException e) {
                    Log.e("XTulator", "Failed to copy asset " + sub + ": " + e.getMessage(), e);
                    throw e;
                }
            }
        }
    }



    private static class XTulatorView extends View {
        private static final int FB_MAX_PIXELS = 1024 * 1024;
        private final XTulatorActivity mActivity;
        private volatile boolean mRunning = false;
        private Thread mRenderThread;
        private Bitmap mBitmap;
        private int[] mPixels;
        private int mFbW = 0;
        private int mFbH = 0;
        private Paint mSpeedPaint = null;

        public XTulatorView(XTulatorActivity activity) {
            super(activity);
            mActivity = activity;
            setFocusable(true);
            setFocusableInTouchMode(true);
            requestFocus();
        }

        @Override
        protected void onSizeChanged(int w, int h, int oldw, int oldh) {
            super.onSizeChanged(w, h, oldw, oldh);
            mActivity.nativeResize(w, h);
        }

        public void startRender() {
            if (mRunning) {
                return;
            }
            mRunning = true;
            mRenderThread = new Thread(new Runnable() {
                @Override
                public void run() {
                    renderLoop();
                }
            });
            mRenderThread.start();
        }

        public void stopRender() {
            mRunning = false;
            if (mRenderThread != null) {
                try {
                    mRenderThread.join(200);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
                mRenderThread = null;
            }
        }

        private void renderLoop() {
            int[] dims = new int[2];
            if (mPixels == null) {
                mPixels = new int[FB_MAX_PIXELS];
            }
            int frameCount = 0;
            while (mRunning) {
                int copied = mActivity.nativeGetFrame(dims, mPixels);
                int w = dims[0];
                int h = dims[1];
                if ((w != mFbW || h != mFbH) && w > 0 && h > 0
                        && (w * (long) h) <= (long) FB_MAX_PIXELS
                        && h >= 200) {
                    if (mBitmap != null) {
                        mBitmap.recycle();
                    }
                    mBitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                    mFbW = w;
                    mFbH = h;
                    Log.i("XTulator", "RenderLoop: new bitmap " + w + "x" + h);
                }
                if (mBitmap != null && mPixels != null && mFbW > 0 && mFbH > 0) {
                    if (copied > 0 && frameCount % 300 == 0) {
                        int firstPix = mPixels[0];
                        int midPix = mPixels[(mFbW * mFbH) / 2];
                        Log.i("XTulator", "RenderLoop: copied " + copied + " pixels, frame " + frameCount
                                + " first=0x" + Integer.toHexString(firstPix)
                                + " mid=0x" + Integer.toHexString(midPix)
                                + " fbW=" + mFbW + " fbH=" + mFbH);
                    }
                    mBitmap.setPixels(mPixels, 0, mFbW, 0, 0, mFbW, mFbH);
                    postInvalidate();
                }
                frameCount++;
                try {
                    Thread.sleep(50);
                } catch (InterruptedException e) {
                    break;
                }
            }
            Log.i("XTulator", "RenderLoop: exiting");
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            Bitmap bitmap = mBitmap;
            int fbW = mFbW;
            int fbH = mFbH;
            if (bitmap != null && fbW > 0 && fbH > 0) {
                int vw = getWidth();
                int vh = getHeight();
                float scale = Math.min((float) vw / (float) fbW, (float) vh / (float) fbH);
                int dw = Math.round(fbW * scale);
                int dh = Math.round(fbH * scale);
                int l = (vw - dw) / 2;
                int t = (vh - dh) / 2;
                canvas.drawBitmap(bitmap, null, new Rect(l, t, l + dw, t + dh), null);
                if (mActivity.isShowSpeed()) {
                    if (mSpeedPaint == null) {
                        mSpeedPaint = new Paint();
                        mSpeedPaint.setTextSize(16);
                        mSpeedPaint.setColor(0xFFFFFFFF);
                        mSpeedPaint.setAntiAlias(false);
                    }
                    String speedStr = mActivity.nativeGetSpeedPct() + "%";
                    if (mActivity.isDynrecEnabled()) {
                        speedStr += " DR:N" + mActivity.nativeGetDynrecNativeBlocks() + " I:" + mActivity.nativeGetDynrecInterpreterInstrs();
                    }
                    float speedW = mSpeedPaint.measureText(speedStr);
                    canvas.drawText(speedStr, vw - speedW - 5, 20, mSpeedPaint);
                }
            }
        }
    }
}
