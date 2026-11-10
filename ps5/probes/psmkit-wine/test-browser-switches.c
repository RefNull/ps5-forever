/* ntdll's title_browser_switches: a profile's Chromium switches are appended to
 * a launch that lacks them, and its feature lists merged into the launch's. */
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned short WCHAR;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define PROCESS_PARAMS_FLAG_NORMALIZED 1
typedef struct { unsigned short Length, MaximumLength; WCHAR *Buffer; } UNICODE_STRING;
typedef struct { unsigned int Flags; UNICODE_STRING ImagePathName, CommandLine; } RTL_USER_PROCESS_PARAMETERS;

static int direct = 1, logs;
static int ps5_direct_enabled(void) { return direct; }
static int sceKernelDebugOutText(int channel, const char *text)
{
    (void)channel;
    assert(!strncmp(text, "wine: title added browser switches to ", 38));
    logs++;
    return 0;
}

#include "browser-switches.inc"

static WCHAR *wide(const char *text)
{
    size_t length = strlen(text);
    WCHAR *out = malloc((length + 1) * sizeof(WCHAR));
    for (size_t i = 0; i <= length; i++) out[i] = (unsigned char)text[i];
    return out;
}

/* The command line the launch gets, or NULL when it is passed through as is. */
static char *launch(const char *config, const char *image, const char *command)
{
    static char result[4096];
    RTL_USER_PROCESS_PARAMETERS params = {.Flags = PROCESS_PARAMS_FLAG_NORMALIZED}, copy, *out;
    WCHAR *image_w = wide(image), *command_w = wide(command), *buffer;
    params.ImagePathName = (UNICODE_STRING){strlen(image) * 2, strlen(image) * 2, image_w};
    params.CommandLine = (UNICODE_STRING){strlen(command) * 2, strlen(command) * 2, command_w};
    setenv("WINEPS5_BROWSER_SWITCHES", config, 1);
    out = title_browser_switches(&params, &copy, &buffer);
    if (out == &params)
    {
        assert(!buffer);
        free(image_w); free(command_w);
        return NULL;
    }
    assert(out == &copy && buffer == out->CommandLine.Buffer);
    assert(out->CommandLine.Length <= out->CommandLine.MaximumLength);
    size_t length = out->CommandLine.Length / sizeof(WCHAR);
    assert(length < sizeof(result));
    for (size_t i = 0; i < length; i++) result[i] = (char)buffer[i];
    result[length] = 0;
    free(buffer); free(image_w); free(command_w);
    return result;
}

#define STEAM_IMAGE "\\??\\C:\\Program Files (x86)\\Steam\\bin\\cef\\cef.win64\\steamwebhelper.exe"
#define BNET_IMAGE "\\??\\C:\\Program Files (x86)\\Battle.net\\Battle.net.exe"
#define SWITCHES " --in-process-gpu --enable-features=NetworkServiceInProcess2" \
    " --disable-features=AudioServiceOutOfProcess --renderer-process-limit=1"

int main(void)
{
    const char *steam = "steamwebhelper.exe" SWITCHES, *bnet = "Battle.net.exe" SWITCHES;
    char *line;

    /* Steam's browser process already carries both feature lists. */
    line = launch(steam, STEAM_IMAGE,
                  "steamwebhelper.exe -lang=en_US --enable-features=PlatformHEVCDecoderSupport "
                  "--disable-features=WinRetrieveSuggestionsOnlyOnDemand,SpareRendererForSitePerProcess /prefetch:1");
    assert(line && !strcmp(line,
        "steamwebhelper.exe -lang=en_US --enable-features=PlatformHEVCDecoderSupport,NetworkServiceInProcess2 "
        "--disable-features=WinRetrieveSuggestionsOnlyOnDemand,SpareRendererForSitePerProcess,AudioServiceOutOfProcess "
        "/prefetch:1 --in-process-gpu --renderer-process-limit=1"));

    /* A launch without feature lists gets the switches appended, as before. */
    line = launch(bnet, BNET_IMAGE, "\"C:\\Program Files (x86)\\Battle.net\\Battle.net.exe\" --updatepid=42");
    assert(line && !strcmp(line, "\"C:\\Program Files (x86)\\Battle.net\\Battle.net.exe\" --updatepid=42" SWITCHES));

    /* Features already listed are not repeated; other set switches keep their value. */
    assert(!launch(steam, STEAM_IMAGE,
                   "w.exe --in-process-gpu --renderer-process-limit=3 "
                   "--enable-features=A,NetworkServiceInProcess2 --disable-features=AudioServiceOutOfProcess,B"));
    line = launch("w.exe --enable-features=A,B,,A,C --renderer-process-limit=1", "C:\\w.exe",
                  "w.exe --enable-features=B --renderer-process-limit=3");
    assert(line && !strcmp(line, "w.exe --enable-features=B,A,C --renderer-process-limit=3"));

    /* An empty list takes no leading comma; a quoted list ends at its quote. */
    line = launch("w.exe --enable-features=X,Y", "C:\\w.exe", "w.exe --enable-features= --z");
    assert(line && !strcmp(line, "w.exe --enable-features=X,Y --z"));
    line = launch("w.exe --disable-features=X", "C:\\w.exe", "w.exe \"--disable-features=A\" --z");
    assert(line && !strcmp(line, "w.exe \"--disable-features=A,X\" --z"));

    /* Chromium reads the last list, so that one is extended. */
    line = launch("w.exe --enable-features=X", "C:\\w.exe",
                  "w.exe --enable-features=A --enable-features=B");
    assert(line && !strcmp(line, "w.exe --enable-features=A --enable-features=B,X"));

    /* Only a whole argument is a list: a longer switch name is not. */
    line = launch("w.exe --enable-features=X", "C:\\w.exe", "w.exe --blink-enable-features=A");
    assert(line && !strcmp(line, "w.exe --blink-enable-features=A --enable-features=X"));

    /* The worst case fills the capacity the profile text reserves. */
    line = launch("w.exe --enable-features=a,b,c,d,e,f,g,h", "C:\\w.exe", "w.exe --enable-features=");
    assert(line && !strcmp(line, "w.exe --enable-features=a,b,c,d,e,f,g,h"));

    /* Chromium subprocesses, other programs and non-title launches pass through. */
    assert(!launch(steam, STEAM_IMAGE, "steamwebhelper.exe --type=renderer --enable-features=A"));
    assert(!launch(steam, "C:\\steam.exe", "steam.exe -gamepadui"));
    direct = 0;
    assert(!launch(steam, STEAM_IMAGE, "steamwebhelper.exe --enable-features=A"));
    direct = 1;

    assert(logs == 8);
    puts("browser switches: append, feature-list merge without duplicates, --type= pass-through PASS");
    return 0;
}
