/*
 * In-tree fuzz harnesses for R, modeled on CPython's
 * Modules/_xxtestfuzz/fuzzer.c.
 *
 * Every fuzz target is a static function `fuzz_<name>(const char *data,
 * size_t size)` in this single translation unit.  The libFuzzer entry
 * point LLVMFuzzerTestOneInput() dispatches to them.  Building with
 *
 *     -D_R_FUZZ_ONE -D_R_FUZZ_fuzz_<name>
 *
 * compiles exactly one target into the binary; this is what OSS-Fuzz
 * does, one binary per name listed in fuzz_tests.txt.  Building without
 * _R_FUZZ_ONE compiles every target and runs them all per input (handy
 * for a quick local smoke test).
 *
 * The harness embeds R once in LLVMFuzzerInitialize() and reuses the
 * initialized interpreter across inputs.  It builds against an
 * unmodified R: the only special handling required is saving R's
 * interactive signal handlers across Rf_initEmbeddedR() and restoring
 * the sanitizer/default handlers afterwards, so that a crash surfaces to
 * the fuzzing engine instead of dropping into R's recovery prompt.
 *
 * The same source is consumable by AFL++ (via its libFuzzer-compatible
 * driver) as well as by libFuzzer.
 *
 * See README.md for how to add a target and how to build/run locally.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define R_NO_REMAP 1
#include <Rembedded.h>
#include <Rinternals.h>
#include <R_ext/Parse.h>

/*
 * Input size caps.  R's parser is recursive descent and can overflow the
 * C stack on deeply nested input past ~64 KB, so the parse target caps
 * lower; the others accept up to 64 KB, beyond which inputs almost always
 * decode to allocation failures rather than interesting coverage.
 */
#define FUZZ_MAX_INPUT_PARSE (16 * 1024)
#define FUZZ_MAX_INPUT       (64 * 1024)

/* Shared scratch buffer for the string-input targets.  Only one target
   runs per input at a time, so a single buffer is safe.  Sized for the
   largest cap, +1 for the NUL terminator. */
static char g_buf[FUZZ_MAX_INPUT + 1];

/* ---------------------------------------------------------------------
 * Signal handling
 *
 * R installs its own handlers for SIGSEGV/SIGILL/SIGFPE/... that enter an
 * interactive recovery prompt instead of terminating, and for SIGINT/
 * SIGUSR1/SIGUSR2/SIGPIPE that suspend or jump to recovery.  Both are
 * fatal for a fuzzer: a sanitizer fault (or UBSAN trap) must produce a
 * crash the engine can see.  We snapshot the handlers in force before
 * Rf_initEmbeddedR() (the sanitizer's deadly-signal handlers, or the
 * defaults) and restore them after init, undoing R's overrides while
 * keeping the sanitizers able to report.
 * ------------------------------------------------------------------- */

static const int saved_sigs[] = {
    SIGILL, SIGFPE, SIGSEGV, SIGBUS, SIGABRT,
    SIGINT, SIGUSR1, SIGUSR2, SIGPIPE
};
#define N_SAVED_SIGS ((int)(sizeof(saved_sigs) / sizeof(saved_sigs[0])))

static struct sigaction saved_actions[N_SAVED_SIGS];

static void save_signals(void)
{
    for (int i = 0; i < N_SAVED_SIGS; i++)
        sigaction(saved_sigs[i], NULL, &saved_actions[i]);
}

static void restore_signals(void)
{
    for (int i = 0; i < N_SAVED_SIGS; i++)
        sigaction(saved_sigs[i], &saved_actions[i], NULL);
}

/* ---------------------------------------------------------------------
 * Evaluation helpers
 * ------------------------------------------------------------------- */

/*
 * eval_safe() - evaluate `call` under R_ToplevelExec.
 *
 * R errors longjmp past the caller's frame.  Without a top-level context
 * that unwinds into a dead stack frame and corrupts the PROTECT stack,
 * yielding spurious crashes.  R_ToplevelExec saves and restores the
 * context.  Returns 1 on success, 0 on R-level error; *out (if non-NULL)
 * receives the result.
 */
typedef struct {
    SEXP call;
    SEXP env;
    SEXP result;
    int  ok;
} eval_ctx_t;

static void do_eval(void *data)
{
    eval_ctx_t *ed = (eval_ctx_t *)data;
    ed->result = Rf_eval(ed->call, ed->env);
    ed->ok = 1;
}

static int eval_safe(SEXP call, SEXP env, SEXP *out)
{
    eval_ctx_t ed = { call, env, R_NilValue, 0 };
    R_ToplevelExec(do_eval, &ed);
    if (out != NULL)
        *out = ed.result;
    return ed.ok;
}

/*
 * eval_text() - parse `src` and evaluate each expression, ignoring
 * errors.  Used for one-time setup (options, sink) and target warmup.
 */
static void eval_text(const char *src)
{
    SEXP text, expr;
    ParseStatus status;

    Rf_protect(text = Rf_mkString(src));
    Rf_protect(expr = R_ParseVector(text, -1, &status, R_NilValue));

    if (status == PARSE_OK)
        for (int i = 0; i < Rf_length(expr); i++)
            eval_safe(VECTOR_ELT(expr, i), R_GlobalEnv, NULL);

    Rf_unprotect(2);
}

/*
 * copy_cstring() - copy `len` bytes into `buf` as a NUL-terminated C
 * string.  `cap` is the maximum payload length, excluding the NUL; `buf`
 * must hold cap+1 bytes.  Returns 0 (rejecting the input) if len > cap or
 * the input contains an embedded NUL.  Rejecting embedded NULs matters
 * because R's string API truncates at the first NUL, silently shrinking
 * the effective corpus.
 */
static int copy_cstring(char *buf, size_t cap, const char *src, size_t len)
{
    if (len > cap)
        return 0;
    if (len > 0 && memchr(src, 0, len) != NULL)
        return 0;
    memcpy(buf, src, len);
    buf[len] = '\0';
    return 1;
}

/* =====================================================================
 * Target: fuzz_parse -- R's parser (R_ParseVector)
 *
 * Passes the input to R_ParseVector without evaluating it.  Targets
 * parser bugs only (crashes, OOB reads, infinite loops).
 * ===================================================================== */

static SEXP g_parse_str;        /* reusable length-1 STRSXP container */

typedef struct {
    SEXP str;
    ParseStatus status;
} parse_ctx_t;

static void do_parse(void *data)
{
    parse_ctx_t *pd = (parse_ctx_t *)data;
    SEXP parsed;
    Rf_protect(parsed = R_ParseVector(pd->str, -1, &pd->status, R_NilValue));
    Rf_unprotect(1);
}

static int init_parse(void)
{
    Rf_protect(g_parse_str = Rf_allocVector(STRSXP, 1));
    R_PreserveObject(g_parse_str);
    Rf_unprotect(1);

    /* Warmup: exercise diverse lexer/parser paths so the first input
       does not diverge in coverage from later ones. */
    static const char *warmup[] = {
        "1+1",
        "x <- function(a, b) a + b",
        "if (TRUE) 'yes' else 'no'",
        "for (i in 1:10) i",
        "list(a=1, b=\"hello\", c=NULL, d=NA)",
        "tryCatch(log(-1), warning = identity)",
        "\\(x) x + 1",
        "1:10 |> rev()",
        "r\"(raw string)\"",
        "f <- function(...) c(..1, ..2)",
        NULL
    };
    parse_ctx_t pd;
    for (int i = 0; warmup[i] != NULL; i++) {
        SET_STRING_ELT(g_parse_str, 0, Rf_mkChar(warmup[i]));
        pd.str = g_parse_str;
        pd.status = PARSE_NULL;
        R_ToplevelExec(do_parse, &pd);
    }
    return 1;
}

static int fuzz_parse(const char *data, size_t size)
{
    if (size == 0 || size > FUZZ_MAX_INPUT_PARSE)
        return 0;
    if (!copy_cstring(g_buf, FUZZ_MAX_INPUT_PARSE, data, size))
        return 0;

    SET_STRING_ELT(g_parse_str, 0, Rf_mkCharLen(g_buf, (int)size));

    parse_ctx_t pd;
    pd.str = g_parse_str;
    pd.status = PARSE_NULL;
    R_ToplevelExec(do_parse, &pd);
    return 0;
}

/* =====================================================================
 * Target: fuzz_grep -- R's regex engines (TRE and PCRE2)
 *
 * Uses the input as a pattern passed to grep() (TRE), grep(perl=TRUE)
 * (PCRE2), and sub() (TRE substitution), matched against a fixed set of
 * diverse haystacks plus the pattern itself.
 * ===================================================================== */

static SEXP g_grep_x;           /* fixed haystacks; slot 11 = pattern */
static SEXP g_grep_pat;         /* reusable length-1 pattern container */
static SEXP g_grep_call_tre;
static SEXP g_grep_call_pcre;
static SEXP g_grep_call_sub;

static int init_grep(void)
{
    SEXP x, pat, replacement, perl_true;
    SEXP call_tre, call_pcre, call_sub;
    SEXP sym_grep = Rf_install("grep");
    SEXP sym_sub  = Rf_install("sub");

    Rf_protect(x = Rf_allocVector(STRSXP, 12));
    SET_STRING_ELT(x,  0, Rf_mkChar("hello world"));
    SET_STRING_ELT(x,  1, Rf_mkChar("foo bar baz 123"));
    SET_STRING_ELT(x,  2, Rf_mkChar("the quick brown fox jumps over the lazy dog"));
    SET_STRING_ELT(x,  3, Rf_mkChar("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"));
    SET_STRING_ELT(x,  4, Rf_mkChar(""));
    /* CJK */
    SET_STRING_ELT(x,  5, Rf_mkCharCE("\xe4\xb8\xad\xe6\x96\x87\xe6\xb5\x8b\xe8\xaf\x95", CE_UTF8));
    /* accented Latin + emoji (UTF-8) */
    SET_STRING_ELT(x,  6, Rf_mkCharCE("caf\xc3\xa9 \xf0\x9f\x91\x8d na\xc3\xafve", CE_UTF8));
    /* decomposed vs precomposed accents */
    SET_STRING_ELT(x,  7, Rf_mkCharCE("e\xcc\x81 vs \xc3\xa9", CE_UTF8));
    /* ZWJ, BOM, RTL text */
    SET_STRING_ELT(x,  8, Rf_mkCharCE("a\xe2\x80\x8d" "b\xef\xbb\xbf\xd8\xa7\xd9\x84\xd8\xb9\xd8\xb1\xd8\xa8\xd9\x8a\xd8\xa9", CE_UTF8));
    /* control characters */
    SET_STRING_ELT(x,  9, Rf_mkChar("line1\nline2\ttab\\backslash\rCR"));
    /* Latin1 high bytes */
    SET_STRING_ELT(x, 10, Rf_mkCharCE("\xe9\xe8\xf1\xfc\xdf", CE_LATIN1));
    /* pattern-as-haystack slot, updated each input */
    SET_STRING_ELT(x, 11, Rf_mkChar(""));

    Rf_protect(pat = Rf_allocVector(STRSXP, 1));
    Rf_protect(replacement = Rf_mkString("X"));
    Rf_protect(perl_true = Rf_ScalarLogical(TRUE));

    /* grep(pattern, x) -- TRE */
    Rf_protect(call_tre = Rf_lang3(sym_grep, pat, x));

    /* grep(pattern, x, perl=TRUE) -- PCRE2 */
    Rf_protect(call_pcre = Rf_lang4(sym_grep, pat, x, perl_true));
    SET_TAG(CDDDR(call_pcre), Rf_install("perl"));

    /* sub(pattern, "X", x) -- TRE substitution */
    Rf_protect(call_sub = Rf_lang4(sym_sub, pat, replacement, x));

    g_grep_x = x;
    g_grep_pat = pat;
    g_grep_call_tre = call_tre;
    g_grep_call_pcre = call_pcre;
    g_grep_call_sub = call_sub;

    /* Preserve the objects we keep direct pointers to; replacement and
       perl_true stay reachable through the preserved call objects. */
    R_PreserveObject(g_grep_x);
    R_PreserveObject(g_grep_pat);
    R_PreserveObject(g_grep_call_tre);
    R_PreserveObject(g_grep_call_pcre);
    R_PreserveObject(g_grep_call_sub);
    Rf_unprotect(7);

    /* Warmup: prime both engines' lazy-init paths (character classes,
       unicode properties, quantifiers, alternation, PCRE2 JIT). */
    static const char *warmup_pats[] = {
        "x", "[[:alpha:]]", "\\d+", "a{2,5}", "foo|bar", "(a)(b)\\1",
        "(?i)hello", "\\p{L}", "^.*$", "\\bword\\b", "(?=.*foo)bar",
        "[a-zA-Z0-9]", "(a+)(b+)\\1\\2.*(?:c|d){2,}[[:digit:]]",
        NULL
    };
    for (const char **p = warmup_pats; *p != NULL; p++) {
        SET_STRING_ELT(g_grep_pat, 0, Rf_mkChar(*p));
        eval_safe(g_grep_call_tre,  R_GlobalEnv, NULL);
        eval_safe(g_grep_call_pcre, R_GlobalEnv, NULL);
        eval_safe(g_grep_call_sub,  R_GlobalEnv, NULL);
    }
    return 1;
}

static int fuzz_grep(const char *data, size_t size)
{
    if (size == 0 || size > FUZZ_MAX_INPUT)
        return 0;
    if (!copy_cstring(g_buf, FUZZ_MAX_INPUT, data, size))
        return 0;

    SEXP cs = Rf_mkCharLen(g_buf, (int)size);
    SET_STRING_ELT(g_grep_pat, 0, cs);
    SET_STRING_ELT(g_grep_x, 11, cs);

    eval_safe(g_grep_call_tre,  R_GlobalEnv, NULL);
    eval_safe(g_grep_call_pcre, R_GlobalEnv, NULL);
    eval_safe(g_grep_call_sub,  R_GlobalEnv, NULL);
    return 0;
}

/* =====================================================================
 * Target: fuzz_unserialize -- R's deserializer (unserialize)
 *
 * Passes raw bytes to unserialize() as a raw vector, exercising the
 * serialization format parser used for .rds/.RData files -- a target
 * users routinely feed untrusted data via readRDS().
 * ===================================================================== */

static SEXP g_unser_call;       /* unserialize(<raw>) with swappable arg */

static int init_unserialize(void)
{
    Rf_protect(g_unser_call = Rf_lang2(Rf_install("unserialize"),
                                       Rf_allocVector(RAWSXP, 1)));
    R_PreserveObject(g_unser_call);
    Rf_unprotect(1);

    /* Warmup with a minimal valid RDS (serialize(NULL, NULL)). */
    SEXP w_raw;
    unsigned char rds[] = "A\n3\n262656\n197888\n254\n";
    Rf_protect(w_raw = Rf_allocVector(RAWSXP, sizeof(rds) - 1));
    memcpy(RAW(w_raw), rds, sizeof(rds) - 1);
    SETCADR(g_unser_call, w_raw);
    eval_safe(g_unser_call, R_GlobalEnv, NULL);
    Rf_unprotect(1);
    return 1;
}

static int fuzz_unserialize(const char *data, size_t size)
{
    if (size == 0 || size > FUZZ_MAX_INPUT)
        return 0;

    SEXP raw_vec;
    Rf_protect(raw_vec = Rf_allocVector(RAWSXP, size));
    memcpy(RAW(raw_vec), data, size);

    SETCADR(g_unser_call, raw_vec);
    eval_safe(g_unser_call, R_GlobalEnv, NULL);

    Rf_unprotect(1);
    return 0;
}

/* =====================================================================
 * Target: fuzz_dcf -- R's DCF reader (read.dcf)
 *
 * Passes the input to read.dcf() through a text connection, exercising
 * R's hand-rolled DCF parser used for DESCRIPTION and PACKAGES files.
 * ===================================================================== */

static int init_dcf(void)
{
    /* Warmup: prime textConnection/read.dcf internal state. */
    int ok;
    SEXP str, tc_call, con;

    Rf_protect(str = Rf_mkString("Package: x\n"));
    Rf_protect(tc_call = Rf_lang2(Rf_install("textConnection"), str));
    ok = eval_safe(tc_call, R_GlobalEnv, &con);
    if (ok && con != R_NilValue) {
        Rf_protect(con);

        SEXP dcf_call;
        Rf_protect(dcf_call = Rf_lang2(Rf_install("read.dcf"), con));
        eval_safe(dcf_call, R_GlobalEnv, NULL);

        SEXP close_call;
        Rf_protect(close_call = Rf_lang2(Rf_install("close"), con));
        eval_safe(close_call, R_GlobalEnv, NULL);

        Rf_unprotect(3);
    }
    Rf_unprotect(2);
    return 1;
}

static int fuzz_dcf(const char *data, size_t size)
{
    if (size == 0 || size > FUZZ_MAX_INPUT)
        return 0;
    if (!copy_cstring(g_buf, FUZZ_MAX_INPUT, data, size))
        return 0;

    SEXP str, tc_call, con;
    Rf_protect(str = Rf_ScalarString(Rf_mkCharLen(g_buf, (int)size)));

    /* con <- textConnection(str) */
    Rf_protect(tc_call = Rf_lang2(Rf_install("textConnection"), str));
    if (!eval_safe(tc_call, R_GlobalEnv, &con) || con == R_NilValue) {
        Rf_unprotect(2);
        return 0;
    }
    Rf_protect(con);

    /* read.dcf(con) */
    SEXP dcf_call;
    Rf_protect(dcf_call = Rf_lang2(Rf_install("read.dcf"), con));
    eval_safe(dcf_call, R_GlobalEnv, NULL);

    /* close(con) */
    SEXP close_call;
    Rf_protect(close_call = Rf_lang2(Rf_install("close"), con));
    eval_safe(close_call, R_GlobalEnv, NULL);

    Rf_unprotect(5);
    return 0;
}

/* =====================================================================
 * libFuzzer entry points
 * ===================================================================== */

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;

    /* Enable experimental pipe-bind (=>) so the parser target can reach
       that code path.  Harmless for the other targets.  Must be set
       before Rf_initEmbeddedR(). */
    setenv("_R_USE_PIPEBIND_", "true", 0);

    static char *r_argv[] = { "R", "--vanilla", "--no-echo", "--no-restore" };
    int r_argc = (int)(sizeof(r_argv) / sizeof(r_argv[0]));

    save_signals();
    Rf_initEmbeddedR(r_argc, r_argv);
    restore_signals();

    /* Suppress warnings (their accumulation perturbs behavior across
       inputs) and sink stdout/stderr so target output stays quiet. */
    eval_text("options(warn = -1)");
    eval_text("sink(file('/dev/null', 'w'));"
              "sink(file('/dev/null', 'w'), type = 'message')");
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const char *input = (const char *)data;

#if !defined(_R_FUZZ_ONE) || defined(_R_FUZZ_fuzz_parse)
    {
        static int initialized = 0;
        if (!initialized) {
            if (!init_parse())
                abort();
            initialized = 1;
        }
        fuzz_parse(input, size);
    }
#endif

#if !defined(_R_FUZZ_ONE) || defined(_R_FUZZ_fuzz_grep)
    {
        static int initialized = 0;
        if (!initialized) {
            if (!init_grep())
                abort();
            initialized = 1;
        }
        fuzz_grep(input, size);
    }
#endif

#if !defined(_R_FUZZ_ONE) || defined(_R_FUZZ_fuzz_unserialize)
    {
        static int initialized = 0;
        if (!initialized) {
            if (!init_unserialize())
                abort();
            initialized = 1;
        }
        fuzz_unserialize(input, size);
    }
#endif

#if !defined(_R_FUZZ_ONE) || defined(_R_FUZZ_fuzz_dcf)
    {
        static int initialized = 0;
        if (!initialized) {
            if (!init_dcf())
                abort();
            initialized = 1;
        }
        fuzz_dcf(input, size);
    }
#endif

    return 0;
}
