package org.d2r.binaryprobe;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Queue;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;

public final class AndroidUpdateCheckerTest {
    private static final String URL = "https://updates.example/latest.json";

    public static void main(String[] args) throws Exception {
        testNewVersionAndNormalization();
        testSameAndOlderVersion();
        testRetryThenSuccess();
        testThreeNetworkFailures();
        testPermissionFailure();
        testRejectedResponses();
        testConcurrentRequestsAreDeduplicated();
        testDisabledConfigurationsDoNotFetch();
        testOpenAndClearInstallUrl();
        System.out.println("AndroidUpdateCheckerTest: PASS");
    }

    private static void testNewVersionAndNormalization() {
        String longNote = "一二三四五六七八九十一二三四五六七八九十甲乙丙丁\n戊己庚辛壬癸";
        Recorder recorder = new Recorder();
        AndroidUpdateChecker checker = checker(7, response(200, json(8, longNote, true, URL)),
                recorder, new ArrayList<>(), Runnable::run);
        checker.requestCheck();
        check(recorder.updates.size() == 1, "new version callback");
        AndroidUpdateChecker.Update update = recorder.updates.get(0);
        check(update.build == 8, "new build");
        check(update.urgent, "urgent flag");
        check(update.note.codePointCount(0, update.note.length()) == 24, "note truncation");
        check(!update.note.contains("\n"), "newline normalization");
        checker.close();
    }

    private static void testSameAndOlderVersion() {
        for (int build : new int[] {7, 6}) {
            Recorder recorder = new Recorder();
            AndroidUpdateChecker checker = checker(7, response(200, json(build, "same", false, URL)),
                    recorder, new ArrayList<>(), Runnable::run);
            checker.requestCheck();
            check(recorder.same == 1, "same or older callback for " + build);
            check(recorder.updates.isEmpty() && recorder.failures.isEmpty(), "same/older only");
            checker.close();
        }
    }

    private static void testRetryThenSuccess() {
        Recorder recorder = new Recorder();
        List<Long> sleeps = new ArrayList<>();
        AtomicInteger attempts = new AtomicInteger();
        AndroidUpdateChecker checker = AndroidUpdateChecker.forTest(7, URL, ignored -> {
            if (attempts.incrementAndGet() < 3) throw new IOException("offline");
            return response(200, json(8, "retry", false, URL));
        }, sleeps::add, Runnable::run, recorder, ignored -> true);
        checker.requestCheck();
        check(attempts.get() == 3, "three attempts");
        check(sleeps.equals(java.util.Arrays.asList(3000L, 6000L)), "retry delays");
        check(recorder.updates.size() == 1, "retry success callback");
        checker.close();
    }

    private static void testThreeNetworkFailures() {
        Recorder recorder = new Recorder();
        List<Long> sleeps = new ArrayList<>();
        AtomicInteger attempts = new AtomicInteger();
        AndroidUpdateChecker checker = AndroidUpdateChecker.forTest(7, URL, ignored -> {
            attempts.incrementAndGet();
            throw new IOException("offline");
        }, sleeps::add, Runnable::run, recorder, ignored -> true);
        checker.requestCheck();
        check(attempts.get() == 3, "failure attempts");
        check(sleeps.equals(java.util.Arrays.asList(3000L, 6000L)), "failure delays");
        check(recorder.failures.equals(java.util.Arrays.asList(
                AndroidUpdateChecker.OTHER_FAILURE)), "other failure");
        checker.close();
    }

    private static void testPermissionFailure() {
        Recorder recorder = new Recorder();
        AndroidUpdateChecker checker = AndroidUpdateChecker.forTest(7, URL, ignored -> {
            throw new IOException("socket failed: EPERM (Operation not permitted)");
        }, ignored -> {}, Runnable::run, recorder, ignored -> true);
        checker.requestCheck();
        check(recorder.failures.equals(java.util.Arrays.asList(
                AndroidUpdateChecker.NOT_PERMITTED)), "permission classification");
        checker.close();
    }

    private static void testRejectedResponses() {
        String valid = json(8, "ok", false, URL);
        assertRejected(response(500, valid), "HTTP non-200");
        assertRejected(response(200, repeat('x', 4097)), "oversized response");
        assertRejected(response(200, "{\"build\":8,\"note\":\"x\",\"urgent\":false}"),
                "missing install_url");
        assertRejected(response(200, "{\"build\":\"8\",\"note\":\"x\",\"urgent\":false,\"install_url\":\"" + URL + "\"}"),
                "build wrong type");
        assertRejected(response(200, "{\"build\":8,\"note\":\"x\",\"urgent\":0,\"install_url\":\"" + URL + "\"}"),
                "urgent wrong type");
        assertRejected(response(200, json(8.5, "x", false, URL)), "fractional build");
        assertRejected(response(200, json(0, "x", false, URL)), "zero build");
        assertRejected(response(200, json(8, "x", false, "http://updates.example/latest.json")),
                "HTTP install URL");
        assertRejected(response(200, json(8, "x", false,
                "itms-services://?action=download-manifest&url=https://updates.example/app.plist")),
                "mobile manifest install URL");
        assertRejected(response(200,
                "{\"build\":8,\"build\":9,\"note\":\"x\",\"urgent\":false,\"install_url\":\"" + URL + "\"}"),
                "duplicate key");
        assertRejected(response(200, "{\"build\":8,\"note\":\"\\uD800\",\"urgent\":false,\"install_url\":\"" + URL + "\"}"),
                "unpaired surrogate");
        assertRejected(new AndroidUpdateChecker.Response(200, new byte[] {
                (byte) 0xC0, (byte) 0xAF
        }), "malformed UTF-8");
    }

    private static void testConcurrentRequestsAreDeduplicated() {
        Recorder recorder = new Recorder();
        AtomicInteger fetches = new AtomicInteger();
        Queue<Runnable> queued = new ArrayDeque<>();
        Executor executor = queued::add;
        AndroidUpdateChecker checker = AndroidUpdateChecker.forTest(7, URL, ignored -> {
            fetches.incrementAndGet();
            return response(200, json(8, "once", false, URL));
        }, ignored -> {}, executor, recorder, ignored -> true);
        checker.requestCheck();
        checker.requestCheck();
        check(queued.size() == 1, "one queued request");
        queued.remove().run();
        check(fetches.get() == 1 && recorder.updates.size() == 1, "deduplicated fetch");
        checker.close();
    }

    private static void testDisabledConfigurationsDoNotFetch() {
        for (Object[] setup : new Object[][] {
                {1, URL}, {7, ""}, {7, "http://updates.example/latest.json"}, {7, null}
        }) {
            Recorder recorder = new Recorder();
            AtomicInteger fetches = new AtomicInteger();
            AndroidUpdateChecker checker = AndroidUpdateChecker.forTest((Integer) setup[0],
                    (String) setup[1], ignored -> {
                        fetches.incrementAndGet();
                        return response(200, json(8, "x", false, URL));
                    }, ignored -> {}, Runnable::run, recorder, ignored -> true);
            checker.requestCheck();
            check(fetches.get() == 0, "disabled update network");
            check(recorder.updates.isEmpty() && recorder.same == 0 && recorder.failures.isEmpty(),
                    "disabled update callback");
            checker.close();
        }
    }

    private static void testOpenAndClearInstallUrl() {
        Recorder recorder = new Recorder();
        List<String> opened = new ArrayList<>();
        Queue<AndroidUpdateChecker.Response> responses = new ArrayDeque<>();
        responses.add(response(200, json(8, "new", false, URL)));
        responses.add(response(200, json(7, "same", false, URL)));
        AndroidUpdateChecker checker = AndroidUpdateChecker.forTest(7, URL,
                ignored -> responses.remove(), ignored -> {}, Runnable::run, recorder, value -> {
                    opened.add(value);
                    return true;
                });
        checker.requestCheck();
        check(checker.open(), "open update URL");
        check(opened.equals(java.util.Arrays.asList(URL)), "opened exact URL");
        checker.requestCheck();
        check(!checker.open(), "same build clears install URL");
        checker.close();
    }

    private static void assertRejected(AndroidUpdateChecker.Response response, String label) {
        Recorder recorder = new Recorder();
        AndroidUpdateChecker checker = checker(7, response, recorder, new ArrayList<>(), Runnable::run);
        checker.requestCheck();
        check(recorder.updates.isEmpty() && recorder.same == 0, label + ": no success");
        check(recorder.failures.equals(java.util.Arrays.asList(
                AndroidUpdateChecker.OTHER_FAILURE)), label + ": failure");
        checker.close();
    }

    private static AndroidUpdateChecker checker(int ownBuild,
            AndroidUpdateChecker.Response response, Recorder recorder,
            List<Long> sleeps, Executor executor) {
        return AndroidUpdateChecker.forTest(ownBuild, URL, ignored -> response,
                sleeps::add, executor, recorder, ignored -> true);
    }

    private static AndroidUpdateChecker.Response response(int status, String body) {
        return new AndroidUpdateChecker.Response(status, body.getBytes(StandardCharsets.UTF_8));
    }

    private static String json(int build, String note, boolean urgent, String installUrl) {
        return json((double) build, note, urgent, installUrl);
    }

    private static String json(double build, String note, boolean urgent, String installUrl) {
        return "{\"build\":" + build + ",\"note\":\"" + escape(note)
                + "\",\"urgent\":" + urgent + ",\"install_url\":\"" + escape(installUrl) + "\"}";
    }

    private static String escape(String value) {
        return value.replace("\\", "\\\\").replace("\"", "\\\"")
                .replace("\n", "\\n").replace("\r", "\\r");
    }

    private static String repeat(char value, int count) {
        StringBuilder output = new StringBuilder(count);
        for (int i = 0; i < count; i++) output.append(value);
        return output.toString();
    }

    private static void check(boolean condition, String label) {
        if (!condition) throw new AssertionError(label);
    }

    private static final class Recorder implements AndroidUpdateChecker.Listener {
        final List<AndroidUpdateChecker.Update> updates = new ArrayList<>();
        final List<Integer> failures = new ArrayList<>();
        int same;

        @Override public void onUpdate(AndroidUpdateChecker.Update update) { updates.add(update); }
        @Override public void onSameOrOlder() { same++; }
        @Override public void onFailure(int kind) { failures.add(kind); }
    }
}
