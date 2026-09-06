// SPDX-License-Identifier: GPL-2.0-only
import com.mydriver.test.*;
import java.io.*;
import java.util.*;
import java.util.concurrent.TimeUnit;
public class ReportLimitsProbe {
  private static final class FixtureProcess extends Process {
    private final InputStream in;
    FixtureProcess(String text) { in = new ByteArrayInputStream(text.getBytes(java.nio.charset.StandardCharsets.UTF_8)); }
    public InputStream getInputStream() { return in; }
    public InputStream getErrorStream() { return new ByteArrayInputStream(new byte[0]); }
    public OutputStream getOutputStream() { return new ByteArrayOutputStream(); }
    public int waitFor() { return 0; }
    public boolean waitFor(long timeout, TimeUnit unit) { return true; }
    public int exitValue() { return 0; }
    public void destroy() {}
    public Process destroyForcibly() { return this; }
  }
  public static void main(String[] args) {
    List<String> lines = new ArrayList<>();
    int count = 0;
    for (TestSpec spec : TestCatalog.INSTANCE.getAll()) {
      for (String key : spec.getChecks()) { lines.add("[PASS] " + key + " fixture"); count++; }
    }
    lines.add("[FAIL] new_unlisted_check fixture failure");
    lines.add("== summary: " + count + " PASS, 1 FAIL, 0 SKIP ==");
    SuiteReport report = new SuiteReport(lines, 1, null);
    long visiblePass = TestCatalog.INSTANCE.getAll().stream().filter(s -> report.result(s.getChecks()).getOutcome() == TestOutcome.PASS).count();
    System.out.println("known_checks=" + count + "; extra_failed_checks=1; visible_pass_rows=" + visiblePass);
    if (visiblePass != 14) throw new AssertionError("Unknown-key limitation was not reproduced");
    String text = "\n".repeat(1100000) + "[PASS] check fixture\n== summary: 1 PASS, 0 FAIL, 0 SKIP ==\n";
    TestResult result = new SuiteRunner(() -> new FixtureProcess(text)).runSuite().result(Arrays.asList("check"));
    System.out.println("newline_heavy_output_chars=" + text.length() + "; outcome=" + result.getOutcome());
    if (result.getOutcome() != TestOutcome.PASS) throw new AssertionError("Line-limit limitation was not reproduced");
  }
}
