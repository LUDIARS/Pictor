package com.ludiars.pictor.demos;

import android.app.Activity;
import android.os.Bundle;
import android.view.Choreographer;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public final class RenderActivity extends Activity implements SurfaceHolder.Callback, Choreographer.FrameCallback {
    static { System.loadLibrary("pictor_demos"); }
    private long renderer;
    private boolean resumed;
    private SurfaceView surface;
    private String shaderDirectory;
    private int demo;
    private static native long createRenderer(Surface surface, String shaderDirectory, int demo);
    private static native void drawFrame(long renderer);
    private static native void destroyRenderer(long renderer);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        demo = getIntent().getIntExtra("demo", 0);
        try {
            File shaders = new File(getFilesDir(), "shaders");
            if (!shaders.isDirectory() && !shaders.mkdirs()) throw new java.io.IOException("Cannot create shader directory");
            for (String name : new String[]{"simple_inst.vert.spv", "simple_inst.frag.spv"}) {
                try (InputStream input = getAssets().open(name);
                     FileOutputStream output = new FileOutputStream(new File(shaders, name))) {
                    byte[] buffer = new byte[8192]; int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                }
            }
            shaderDirectory = shaders.getAbsolutePath();
            surface = new SurfaceView(this);
            surface.getHolder().addCallback(this);
            setContentView(surface);
        } catch (Exception error) { showError(error); }
    }

    private void stopRenderer() {
        Choreographer.getInstance().removeFrameCallback(this);
        if (renderer != 0) { destroyRenderer(renderer); renderer = 0; }
    }
    private void startRenderer() {
        if (!resumed || surface == null || !surface.getHolder().getSurface().isValid()) return;
        stopRenderer();
        try {
            renderer = createRenderer(surface.getHolder().getSurface(), shaderDirectory, demo);
            if (renderer == 0) throw new IllegalStateException("Native renderer was not created");
            Choreographer.getInstance().postFrameCallback(this);
        } catch (RuntimeException error) { showError(error); }
    }
    private void showError(Exception error) {
        stopRenderer();
        TextView text = new TextView(this);
        text.setText("Pictor demo unavailable\n" + error.getMessage());
        setContentView(text);
        surface = null;
    }
    @Override protected void onResume() { super.onResume(); resumed = true; startRenderer(); }
    @Override protected void onPause() { resumed = false; stopRenderer(); super.onPause(); }
    @Override public void surfaceCreated(SurfaceHolder holder) { startRenderer(); }
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { startRenderer(); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { stopRenderer(); }
    @Override public void doFrame(long nanos) {
        if (!resumed || renderer == 0) return;
        try { drawFrame(renderer); Choreographer.getInstance().postFrameCallback(this); }
        catch (RuntimeException error) { showError(error); }
    }
}
