package mglprobe;

import java.lang.reflect.Method;
import java.nio.Buffer;
import java.nio.ByteBuffer;

public final class TexProbeRuntime {
    private static final boolean STACK = !"0".equals(System.getProperty("mgl.texprobe.stack", "1"));
    private static final int MAX_LOG = Integer.getInteger("mgl.texprobe.max", 80);
    private static final int TARGET_W = Integer.getInteger("mgl.texprobe.width", 512);
    private static final int TARGET_H = Integer.getInteger("mgl.texprobe.height", 512);
    private static int seen;

    private TexProbeRuntime() {}

    public static void logTexSubImage2DBuffer(String owner, String desc,
            int target, int level, int xoffset, int yoffset, int width, int height, int format, int type,
            Buffer pixels) {
        if (!shouldLog(width, height)) return;
        int id = nextId();
        if (id < 0) return;

        long address = memAddressSafe(pixels);
        int pos = pixels == null ? -1 : pixels.position();
        int lim = pixels == null ? -1 : pixels.limit();
        int cap = pixels == null ? -1 : pixels.capacity();
        boolean direct = pixels != null && pixels.isDirect();
        String cls = pixels == null ? "null" : pixels.getClass().getName();
        String dump = dumpBufferHead(pixels, 32);
        long hash = hashBufferHead(pixels, 256);
        boolean zero = looksZero(pixels, 256);

        System.err.printf(
                "MGLJ TEXPROBE #%d %s%s target=0x%x level=%d off=(%d,%d) size=%dx%d format=0x%x type=0x%x bufferClass=%s direct=%s pos=%d lim=%d cap=%d addr=0x%x hash256=0x%016x zero256=%s head=%s%n",
                id, owner, desc, target, level, xoffset, yoffset, width, height, format, type,
                cls, direct, pos, lim, cap, address, hash, zero, dump);
        logStack(id);
    }

    public static void logTexSubImage2DAddress(String owner, String desc,
            int target, int level, int xoffset, int yoffset, int width, int height, int format, int type,
            long pixels) {
        if (!shouldLog(width, height)) return;
        int id = nextId();
        if (id < 0) return;
        System.err.printf(
                "MGLJ TEXPROBE #%d %s%s target=0x%x level=%d off=(%d,%d) size=%dx%d format=0x%x type=0x%x rawAddressOrPboOffset=0x%x%n",
                id, owner, desc, target, level, xoffset, yoffset, width, height, format, type, pixels);
        logStack(id);
    }

    private static boolean shouldLog(int width, int height) {
        return TARGET_W <= 0 || TARGET_H <= 0 || (width == TARGET_W && height == TARGET_H);
    }

    private static synchronized int nextId() {
        if (seen >= MAX_LOG) return -1;
        return ++seen;
    }

    private static void logStack(int id) {
        if (!STACK) return;
        StackTraceElement[] stack = Thread.currentThread().getStackTrace();
        System.err.println("MGLJ TEXPROBE #" + id + " stack:");
        for (int i = 2; i < Math.min(stack.length, 28); i++) {
            String s = stack[i].toString();
            if (s.startsWith("org.lwjgl.opengl.GL11C.") ||
                s.startsWith("org.lwjgl.opengl.GL12C.") ||
                s.startsWith("org.lwjgl.opengl.GL45C.") ||
                s.startsWith("com.mojang.") ||
                s.startsWith("net.minecraft.") ||
                s.matches("^[a-z]{2,4}\\..*|^[a-z]{2,4}\\(.*")) {
                System.err.println("  at " + s);
            }
        }
    }

    private static long memAddressSafe(Buffer b) {
        if (b == null) return 0L;
        try {
            Class<?> mu = Class.forName("org.lwjgl.system.MemoryUtil");
            Method m = mu.getMethod("memAddressSafe", Buffer.class);
            Object v = m.invoke(null, b);
            return ((Long)v).longValue();
        } catch (Throwable ignored) {
            return 0L;
        }
    }

    private static String dumpBufferHead(Buffer b, int maxBytes) {
        if (!(b instanceof ByteBuffer)) return b == null ? "-" : "non-byte-buffer";
        ByteBuffer bb = ((ByteBuffer)b).duplicate();
        int n = Math.min(Math.min(maxBytes, bb.remaining()), 64);
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < n; i++) {
            if (i > 0) sb.append(':');
            int v = bb.get(bb.position() + i) & 0xff;
            if (v < 16) sb.append('0');
            sb.append(Integer.toHexString(v));
        }
        return sb.toString();
    }

    private static long hashBufferHead(Buffer b, int maxBytes) {
        if (!(b instanceof ByteBuffer)) return 0L;
        ByteBuffer bb = ((ByteBuffer)b).duplicate();
        int n = Math.min(maxBytes, bb.remaining());
        long h = 0xcbf29ce484222325L;
        for (int i = 0; i < n; i++) {
            h ^= (bb.get(bb.position() + i) & 0xff);
            h *= 0x100000001b3L;
        }
        return h;
    }

    private static boolean looksZero(Buffer b, int maxBytes) {
        if (!(b instanceof ByteBuffer)) return false;
        ByteBuffer bb = ((ByteBuffer)b).duplicate();
        int n = Math.min(maxBytes, bb.remaining());
        if (n <= 0) return false;
        for (int i = 0; i < n; i++) {
            if (bb.get(bb.position() + i) != 0) return false;
        }
        return true;
    }
}
