package org.d2r.binaryprobe;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.math.BigDecimal;
import java.net.HttpURLConnection;
import java.net.URI;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;

/** Android transport for the cross-platform update notice protocol. */
final class AndroidUpdateChecker {
    static final int MAX_RESPONSE_BYTES = 4096;
    static final int REQUEST_TIMEOUT_MS = 10000;
    static final int TRIES = 3;
    static final int NOT_PERMITTED = 1;
    static final int OTHER_FAILURE = 2;
    private static final int MAX_JSON_DEPTH = 32;

    interface Fetcher { Response fetch(String url) throws IOException; }
    interface Sleeper { void sleep(long millis) throws InterruptedException; }
    interface Listener {
        void onUpdate(Update update);
        void onSameOrOlder();
        void onFailure(int kind);
    }
    interface Opener { boolean open(String httpsUrl); }

    static final class Response {
        final int status;
        final byte[] body;
        Response(int status, byte[] body) {
            this.status = status;
            this.body = body == null ? new byte[0] : body;
        }
    }

    static final class Update {
        final int build;
        final String note;
        final boolean urgent;
        final String installUrl;
        Update(int build, String note, boolean urgent, String installUrl) {
            this.build = build;
            this.note = note;
            this.urgent = urgent;
            this.installUrl = installUrl;
        }
    }

    private final int ownBuild;
    private final String updateUrl;
    private final Fetcher fetcher;
    private final Sleeper sleeper;
    private final Executor executor;
    private final Listener listener;
    private final Opener opener;
    private final AtomicBoolean inFlight = new AtomicBoolean();
    private volatile String installUrl;
    private volatile boolean closed;

    private AndroidUpdateChecker(int ownBuild, String updateUrl, Fetcher fetcher,
            Sleeper sleeper, Executor executor, Listener listener, Opener opener) {
        this.ownBuild = ownBuild;
        this.updateUrl = updateUrl;
        this.fetcher = fetcher;
        this.sleeper = sleeper;
        this.executor = executor;
        this.listener = listener;
        this.opener = opener;
    }

    static AndroidUpdateChecker create(int ownBuild, String updateUrl,
            Listener listener, Opener opener) {
        ExecutorService worker = Executors.newSingleThreadExecutor(task -> {
            Thread thread = new Thread(task, "d2r-update-check");
            thread.setDaemon(true);
            return thread;
        });
        return new AndroidUpdateChecker(ownBuild, updateUrl, new HttpFetcher(),
                Thread::sleep, worker, listener, opener);
    }

    static AndroidUpdateChecker forTest(int ownBuild, String updateUrl, Fetcher fetcher,
            Sleeper sleeper, Executor executor, Listener listener, Opener opener) {
        return new AndroidUpdateChecker(ownBuild, updateUrl, fetcher, sleeper,
                executor, listener, opener);
    }

    static boolean validHttpsUrl(String value) {
        if (value == null || value.isEmpty()) return false;
        try {
            URI uri = URI.create(value);
            return "https".equalsIgnoreCase(uri.getScheme())
                    && uri.getHost() != null && !uri.getHost().isEmpty()
                    && uri.getRawUserInfo() == null;
        } catch (IllegalArgumentException ignored) {
            return false;
        }
    }

    void requestCheck() {
        if (closed || ownBuild <= 1 || !validHttpsUrl(updateUrl)) return;
        if (!inFlight.compareAndSet(false, true)) return;
        try {
            executor.execute(this::runCheck);
        } catch (RejectedExecutionException rejected) {
            inFlight.set(false);
        }
    }

    boolean open() {
        String url = installUrl;
        return !closed && url != null && opener != null && opener.open(url);
    }

    void close() {
        closed = true;
        installUrl = null;
        if (executor instanceof ExecutorService) ((ExecutorService) executor).shutdownNow();
    }

    private void runCheck() {
        int lastFailure = OTHER_FAILURE;
        boolean permissionFailure = false;
        try {
            for (int attempt = 1; attempt <= TRIES && !closed; attempt++) {
                try {
                    Response response = fetcher.fetch(updateUrl);
                    if (response == null) throw new IOException("missing response");
                    if (response.status != 200) throw new IOException("HTTP " + response.status);
                    if (response.body.length > MAX_RESPONSE_BYTES) {
                        throw new IOException("response exceeds " + MAX_RESPONSE_BYTES + " bytes");
                    }
                    Update update = parseResponse(response.body);
                    if (closed) return;
                    if (update.build > ownBuild) {
                        installUrl = update.installUrl;
                        listener.onUpdate(update);
                    } else {
                        installUrl = null;
                        listener.onSameOrOlder();
                    }
                    return;
                } catch (IOException | RuntimeException failure) {
                    lastFailure = classifyFailure(failure);
                    permissionFailure |= lastFailure == NOT_PERMITTED;
                }
                if (attempt < TRIES) {
                    if (closed) return;
                    sleeper.sleep(3000L * attempt);
                }
            }
            if (!closed) listener.onFailure(permissionFailure ? NOT_PERMITTED : lastFailure);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            if (!closed) listener.onFailure(permissionFailure ? NOT_PERMITTED : lastFailure);
        } finally {
            inFlight.set(false);
        }
    }

    private static int classifyFailure(Throwable failure) {
        for (Throwable current = failure; current != null; current = current.getCause()) {
            if (current instanceof SecurityException) return NOT_PERMITTED;
            String message = current.getMessage();
            if (message == null) continue;
            String lower = message.toLowerCase(java.util.Locale.ROOT);
            if (lower.contains("permission denied") || lower.contains("access denied")
                    || lower.contains("operation not permitted")
                    || lower.contains("eacces") || lower.contains("eperm")) {
                return NOT_PERMITTED;
            }
        }
        return OTHER_FAILURE;
    }

    private static Update parseResponse(byte[] body) throws IOException {
        if (body.length == 0) throw new IOException("empty response");
        String text;
        try {
            text = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(body)).toString();
        } catch (CharacterCodingException invalidUtf8) {
            throw new IOException("response is not strict UTF-8", invalidUtf8);
        }
        Object value = new JsonParser(text).parse();
        if (!(value instanceof Map)) throw new IOException("version file is not a JSON object");
        Map<?, ?> object = (Map<?, ?>) value;
        Object buildValue = object.get("build");
        Object noteValue = object.get("note");
        Object urgentValue = object.get("urgent");
        Object installValue = object.get("install_url");
        if (!(buildValue instanceof BigDecimal) || !(noteValue instanceof String)
                || !(urgentValue instanceof Boolean) || !(installValue instanceof String)) {
            throw new IOException("version file fields are missing or have wrong types");
        }
        final int build;
        try {
            build = ((BigDecimal) buildValue).intValueExact();
        } catch (ArithmeticException | NumberFormatException invalidBuild) {
            throw new IOException("build is not an integer", invalidBuild);
        }
        if (build < 1 || build >= 2000000000) throw new IOException("build is out of range");
        String installUrl = (String) installValue;
        if (!validHttpsUrl(installUrl)) throw new IOException("install_url is not HTTPS");
        return new Update(build, normalizeNote((String) noteValue),
                (Boolean) urgentValue, installUrl);
    }

    private static String normalizeNote(String note) {
        StringBuilder normalized = new StringBuilder();
        int emitted = 0;
        for (int offset = 0; offset < note.length() && emitted < 24;) {
            int point = note.codePointAt(offset);
            offset += Character.charCount(point);
            if (point == '\n' || point == '\r' || point == 0x85
                    || point == 0x2028 || point == 0x2029) {
                point = ' ';
            }
            normalized.appendCodePoint(point);
            emitted++;
        }
        return normalized.toString();
    }

    static final class HttpFetcher implements Fetcher {
        @Override public Response fetch(String value) throws IOException {
            String current=value;
            for (int redirects=0;;redirects++) {
                if (!validHttpsUrl(current)) throw new IOException("update URL is not HTTPS");
                HttpURLConnection connection=null;
                try {
                    connection=(HttpURLConnection)new URL(current).openConnection();
                    connection.setRequestMethod("GET");
                    connection.setConnectTimeout(REQUEST_TIMEOUT_MS);
                    connection.setReadTimeout(REQUEST_TIMEOUT_MS);
                    connection.setUseCaches(false);
                    connection.setInstanceFollowRedirects(false);
                    connection.setRequestProperty("Accept", "application/json");
                    connection.setRequestProperty("Cache-Control", "no-cache");
                    connection.setRequestProperty("User-Agent", "D2R-Android-update-check");
                    int status=connection.getResponseCode();
                    if (status==301||status==302||status==303||status==307||status==308) {
                        if (redirects>=3) throw new IOException("too many update redirects");
                        String location=connection.getHeaderField("Location");
                        if (location==null||location.isEmpty()) throw new IOException("update redirect has no location");
                        current=new URL(new URL(current),location).toString();
                        if (!validHttpsUrl(current)) throw new IOException("update redirect left HTTPS");
                        continue;
                    }
                    byte[] body=status==200?readAtMost(connection.getInputStream()):new byte[0];
                    return new Response(status,body);
                } catch (ClassCastException|java.net.MalformedURLException invalidUrl) {
                    throw new IOException("update URL is malformed",invalidUrl);
                } finally {
                    if (connection!=null) connection.disconnect();
                }
            }
        }

        private static byte[] readAtMost(InputStream input) throws IOException {
            try (InputStream stream = input; ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                byte[] chunk = new byte[512];
                int count;
                while (output.size() <= MAX_RESPONSE_BYTES
                        && (count = stream.read(chunk)) != -1) {
                    output.write(chunk, 0, Math.min(count,
                            MAX_RESPONSE_BYTES + 1 - output.size()));
                }
                return output.toByteArray();
            }
        }
    }

    private static final class JsonParser {
        private final String input;
        private int index;

        JsonParser(String input) { this.input = input; }

        Object parse() throws IOException {
            skipWhitespace();
            Object value = parseValue(0);
            skipWhitespace();
            if (index != input.length()) error("trailing data");
            return value;
        }

        private Object parseValue(int depth) throws IOException {
            if (depth > MAX_JSON_DEPTH) error("nesting is too deep");
            skipWhitespace();
            if (index >= input.length()) error("unexpected end");
            char current = input.charAt(index);
            if (current == '{') return parseObject(depth + 1);
            if (current == '[') return parseArray(depth + 1);
            if (current == '"') return parseString();
            if (current == 't') { expect("true"); return Boolean.TRUE; }
            if (current == 'f') { expect("false"); return Boolean.FALSE; }
            if (current == 'n') { expect("null"); return null; }
            if (current == '-' || (current >= '0' && current <= '9')) return parseNumber();
            error("unexpected token");
            return null;
        }

        private Map<String, Object> parseObject(int depth) throws IOException {
            index++;
            Map<String, Object> object = new HashMap<>();
            Set<String> keys = new HashSet<>();
            skipWhitespace();
            if (consume('}')) return object;
            while (true) {
                skipWhitespace();
                if (index >= input.length() || input.charAt(index) != '"') error("object key is not a string");
                String key = parseString();
                if (!keys.add(key)) error("duplicate key: " + key);
                skipWhitespace();
                if (!consume(':')) error("expected colon");
                object.put(key, parseValue(depth));
                skipWhitespace();
                if (consume('}')) return object;
                if (!consume(',')) error("expected comma");
            }
        }

        private List<Object> parseArray(int depth) throws IOException {
            index++;
            List<Object> array = new ArrayList<>();
            skipWhitespace();
            if (consume(']')) return array;
            while (true) {
                array.add(parseValue(depth));
                skipWhitespace();
                if (consume(']')) return array;
                if (!consume(',')) error("expected comma");
            }
        }

        private String parseString() throws IOException {
            if (!consume('"')) error("expected string");
            StringBuilder output = new StringBuilder();
            while (index < input.length()) {
                char current = input.charAt(index++);
                if (current == '"') return output.toString();
                if (current < 0x20) error("control character in string");
                if (current == '\\') {
                    if (index >= input.length()) error("unfinished escape");
                    char escaped = input.charAt(index++);
                    switch (escaped) {
                        case '"': output.append('"'); break;
                        case '\\': output.append('\\'); break;
                        case '/': output.append('/'); break;
                        case 'b': output.append('\b'); break;
                        case 'f': output.append('\f'); break;
                        case 'n': output.append('\n'); break;
                        case 'r': output.append('\r'); break;
                        case 't': output.append('\t'); break;
                        case 'u': appendUnicodeEscape(output); break;
                        default: error("invalid escape");
                    }
                } else if (Character.isHighSurrogate(current)) {
                    if (index >= input.length() || !Character.isLowSurrogate(input.charAt(index))) {
                        error("unpaired high surrogate");
                    }
                    output.append(current).append(input.charAt(index++));
                } else {
                    if (Character.isLowSurrogate(current)) error("unpaired low surrogate");
                    output.append(current);
                }
            }
            error("unterminated string");
            return null;
        }

        private void appendUnicodeEscape(StringBuilder output) throws IOException {
            char first = readHexUnit();
            if (Character.isHighSurrogate(first)) {
                if (index + 1 >= input.length() || input.charAt(index) != '\\'
                        || input.charAt(index + 1) != 'u') {
                    error("unpaired high surrogate");
                }
                index += 2;
                char second = readHexUnit();
                if (!Character.isLowSurrogate(second)) error("unpaired high surrogate");
                output.append(first).append(second);
            } else {
                if (Character.isLowSurrogate(first)) error("unpaired low surrogate");
                output.append(first);
            }
        }

        private char readHexUnit() throws IOException {
            if (index + 4 > input.length()) error("short unicode escape");
            int value = 0;
            for (int i = 0; i < 4; i++) {
                int digit = Character.digit(input.charAt(index++), 16);
                if (digit < 0) error("invalid unicode escape");
                value = value * 16 + digit;
            }
            return (char) value;
        }

        private BigDecimal parseNumber() throws IOException {
            int start = index;
            if (consume('-') && index >= input.length()) error("short number");
            if (consume('0')) {
                if (index < input.length() && Character.isDigit(input.charAt(index))) {
                    error("leading zero");
                }
            } else {
                if (index >= input.length() || input.charAt(index) < '1' || input.charAt(index) > '9') {
                    error("invalid number");
                }
                while (index < input.length() && Character.isDigit(input.charAt(index))) index++;
            }
            if (consume('.')) {
                int fractionStart = index;
                while (index < input.length() && Character.isDigit(input.charAt(index))) index++;
                if (fractionStart == index) error("invalid fraction");
            }
            if (index < input.length() && (input.charAt(index) == 'e' || input.charAt(index) == 'E')) {
                index++;
                if (index < input.length() && (input.charAt(index) == '+' || input.charAt(index) == '-')) index++;
                int exponentStart = index;
                while (index < input.length() && Character.isDigit(input.charAt(index))) index++;
                if (exponentStart == index) error("invalid exponent");
            }
            try {
                return new BigDecimal(input.substring(start, index));
            } catch (NumberFormatException invalidNumber) {
                throw new IOException("invalid number", invalidNumber);
            }
        }

        private void expect(String token) throws IOException {
            if (!input.regionMatches(index, token, 0, token.length())) error("invalid token");
            index += token.length();
        }

        private boolean consume(char expected) {
            if (index < input.length() && input.charAt(index) == expected) {
                index++;
                return true;
            }
            return false;
        }

        private void skipWhitespace() {
            while (index < input.length()) {
                char current = input.charAt(index);
                if (current != ' ' && current != '\t' && current != '\n' && current != '\r') return;
                index++;
            }
        }

        private void error(String message) throws IOException {
            throw new IOException("invalid JSON at " + index + ": " + message);
        }
    }
}
