package com.example.libvideocomponent;

import android.content.Context;
import android.graphics.ImageFormat;
import android.media.AudioAttributes;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaPlayer;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;

import java.nio.ByteBuffer;
import java.util.Objects;

/**
 * Owns one MediaPlayer and decodes its video track into an ImageReader surface.
 * Frames are handed to native code once per image; native code converts YUV to
 * RGBA and publishes through the HuxerUI external texture.
 *
 * All public methods are called from the UI thread; media callbacks run on the
 * private handler thread. Every entry point synchronizes on this instance and
 * checks {@link #released} so a dispose from the UI thread is race free.
 */
final class VideoPlayer {
    // Status codes shared with video_android.cpp.
    static final int STATUS_IDLE = 0;
    static final int STATUS_PREPARING = 1;
    static final int STATUS_PLAYING = 2;
    static final int STATUS_PAUSED = 3;
    static final int STATUS_COMPLETED = 4;
    static final int STATUS_FAILED = 5;

    private final long bridge;
    private final HandlerThread thread;
    private final Handler handler;
    private final String source;
    private final boolean loop;

    private MediaPlayer player;
    private ImageReader reader;
    private boolean released;
    private boolean prepared;
    private boolean wantPlaying;

    VideoPlayer(Context context, long bridge, String source, boolean autoPlay, boolean muted, boolean loop) {
        this.bridge = bridge;
        this.source = source;
        this.loop = loop;
        this.wantPlaying = autoPlay;
        Objects.requireNonNull(context);

        thread = new HandlerThread("HuxerUI-VideoPlayer");
        thread.start();
        handler = new Handler(thread.getLooper());
        handler.post(this::open);
    }

    synchronized void setPlaying(boolean playing) {
        if (released) {
            return;
        }
        final boolean changed = wantPlaying != playing;
        wantPlaying = playing;
        if (changed) {
            handler.post(() -> applyPlaying(playing));
        }
    }

    synchronized void dispose() {
        if (released) {
            return;
        }
        released = true;
        handler.post(this::release);
    }

    private void open() {
        reportStatus(STATUS_PREPARING, null);
        try {
            final MediaPlayer created = new MediaPlayer();
            created.setAudioAttributes(
                    new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                            .build());
            created.setDataSource(source);
            created.setLooping(loop);
            created.setOnPreparedListener(this::onPrepared);
            created.setOnCompletionListener(this::onCompletion);
            created.setOnErrorListener(this::onError);
            synchronized (this) {
                if (released) {
                    created.release();
                    return;
                }
                player = created;
            }
            created.prepareAsync();
        } catch (Exception failure) {
            fail("open failed: " + failure);
        }
    }

    private synchronized void onPrepared(MediaPlayer preparedPlayer) {
        if (released || player != preparedPlayer) {
            return;
        }
        final int width = preparedPlayer.getVideoWidth();
        final int height = preparedPlayer.getVideoHeight();
        if (width <= 0 || height <= 0) {
            fail("the source has no video track");
            return;
        }
        try {
            final ImageReader created = ImageReader.newInstance(width, height, ImageFormat.YUV_420_888, 3);
            created.setOnImageAvailableListener(reader -> onImage(), handler);
            preparedPlayer.setSurface(created.getSurface());
            reader = created;
        } catch (Exception failure) {
            fail("frame surface failed: " + failure);
            return;
        }
        prepared = true;
        nativeOnPrepared(bridge, width, height);
        if (wantPlaying) {
            preparedPlayer.start();
            reportStatus(STATUS_PLAYING, null);
        } else {
            reportStatus(STATUS_PAUSED, null);
        }
    }

    private synchronized void applyPlaying(boolean playing) {
        if (released || player == null || !prepared) {
            return;
        }
        try {
            if (playing) {
                if (!player.isPlaying()) {
                    player.start();
                    reportStatus(STATUS_PLAYING, null);
                }
            } else if (player.isPlaying()) {
                player.pause();
                reportStatus(STATUS_PAUSED, null);
            }
        } catch (Exception failure) {
            fail("play state failed: " + failure);
        }
    }

    private synchronized void onCompletion(MediaPlayer completedPlayer) {
        if (released || player != completedPlayer) {
            return;
        }
        reportStatus(STATUS_COMPLETED, null);
    }

    private synchronized boolean onError(MediaPlayer failedPlayer, int what, int extra) {
        if (released || player != failedPlayer) {
            return true;
        }
        fail("media error " + what + "/" + extra);
        return true;
    }

    private void onImage() {
        final Image image;
        synchronized (this) {
            if (released || reader == null) {
                return;
            }
            image = reader.acquireLatestImage();
        }
        if (image == null) {
            return;
        }
        try {
            final Image.Plane[] planes = image.getPlanes();
            // slice() re-bases each plane buffer at its current position, so
            // native code sees the first pixel regardless of device layout.
            nativeFrame(
                    bridge,
                    planes[0].getBuffer().slice(),
                    planes[1].getBuffer().slice(),
                    planes[2].getBuffer().slice(),
                    planes[0].getRowStride(),
                    planes[1].getRowStride(),
                    planes[2].getRowStride(),
                    planes[0].getPixelStride(),
                    planes[1].getPixelStride(),
                    planes[2].getPixelStride(),
                    image.getWidth(),
                    image.getHeight());
        } finally {
            image.close();
        }
    }

    private synchronized void release() {
        if (reader != null) {
            try {
                reader.close();
            } catch (Exception ignored) {
            }
            reader = null;
        }
        if (player != null) {
            try {
                player.reset();
                player.release();
            } catch (Exception ignored) {
            }
            player = null;
        }
        // No further native callbacks can arrive after this point.
        nativeDestroyed(bridge);
        if (Looper.myLooper() == thread.getLooper()) {
            Looper.myLooper().quitSafely();
        }
    }

    private void fail(String message) {
        reportStatus(STATUS_FAILED, message);
    }

    private void reportStatus(int status, String message) {
        nativeStatus(bridge, status, message);
    }

    private static native void nativeOnPrepared(long bridge, int width, int height);

    private static native void nativeFrame(
            long bridge,
            ByteBuffer y,
            ByteBuffer u,
            ByteBuffer v,
            int yRowStride,
            int uRowStride,
            int vRowStride,
            int yPixelStride,
            int uPixelStride,
            int vPixelStride,
            int width,
            int height);

    private static native void nativeStatus(long bridge, int status, String message);

    private static native void nativeDestroyed(long bridge);

    static {
        // The hosting library loads with the HuxerUI runtime; no extra loader needed.
    }
}
