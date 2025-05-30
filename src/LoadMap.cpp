////////////////////////////////////////////////////////////////////////////////
/// @file LoadMap.cpp
///     Implementation of an IDA plugin, which loads a VC++/BCC map file.
/// @par Purpose:
///     An IDA plugin, which loads a VC/Borland/Dede map file into IDA Database.
///     Based on the idea of loadmap plugin by Toshiyuki Tega.
/// @author TQN <truong_quoc_ngan@yahoo.com>
/// @author TL <mefistotelis@gmail.com>
/// @date 2004.09.11 - 2018.11.08
/// @version 1.3 - 2018.11.08 - Compiling in VS2010, SDK from IDA 7.0
/// @version 1.2 - 2012.07.18 - Loading GCC MAP files, compiling in IDA 6.2
/// @version 1.1 - 2011.09.13 - Loading Watcom MAP files, compiling in IDA 6.1
/// @version 1.0 - 2004.09.11 - Initial release
/// @par  Copying and copyrights:
///     This program is free software; you can redistribute it and/or modify
///     it under the terms of the GNU General Public License as published by
///     the Free Software Foundation; either version 2 of the License, or
///     (at your option) any later version.
///     IDA Pro SDK by Hex-rays is required to use this software; that
///     SDK has more complex licensing situation, and is not under GPL.
////////////////////////////////////////////////////////////////////////////////
#define PLUG_VERSION "1.4"
//  standard library headers.
#include <cstdio>
// Makes gcc stdlib to not define non-underscored versions of non-ANSI functions (ie memicmp, strlwr)
#define _NO_OLDNAMES
#include <cstring>
#undef _NO_OLDNAMES

//  other headers.
#include  "MAPReader.h"
#include "stdafx.h"

//#define USE_STANDARD_FILE_FUNCTIONS
//#define USE_DANGEROUS_FUNCTIONS

// IDA SDK Header Files
#include <ida.hpp>
#include <idp.hpp>
#include <loader.hpp>
#include <kernwin.hpp>
#include <diskio.hpp>
#include <bytes.hpp>
#include <name.hpp>
#include <entry.hpp>
#include <fpro.h>
#include <err.h> // for qerrstr()
#include <prodir.h> // just for MAXPATH

#if IDA_SDK_VERSION >= 800 && defined(HAS_IDA_QT_DEV_LIB)
// Hex-rays modified Qt is not a part of IDA SDK, needs to be dowloaded separately
#include <QGuiApplication>
#endif

typedef struct _tagPLUGIN_OPTIONS {
    int bNameApply;    //< true - apply to name, false - apply to comment
    int bReplace;      //< replace the existing name or comment
    int bVerbose;      //< show detail messages
} PLUGIN_OPTIONS;

const size_t g_minLineLen = 14; // For a "xxxx:xxxxxxxx " line

/// @brief Global variable for options of plugin
static PLUGIN_OPTIONS g_options = { 0 };

static const cfgopt_t g_optsinfo[] =
{
    cfgopt_t("NAME_APPLY", &g_options.bNameApply, 0, 1),
    cfgopt_t("REPLACE_EXISTING", &g_options.bReplace, 0, 1),
    cfgopt_t("VERBOSE_MESSAGES", &g_options.bVerbose, 0, 1),
};

////////////////////////////////////////////////////////////////////////////////
/// @name Ini Section and Key names
/// @{
static char g_szLoadMapSection[] = "LoadMap";
static char g_szOptionsKey[] = "Options";
/// @}

#ifdef __EA64__
void linearAddressToSymbolAddr(MapFile::MAPSymbol &sym, unsigned long long linear_addr)
#else
void linearAddressToSymbolAddr(MapFile::MAPSymbol &sym, unsigned long linear_addr)
#endif
{
    sym.seg = get_segm_num(linear_addr);
    segment_t * sseg = getnseg((int) sym.seg);
    if (sseg != NULL)
        sym.addr = linear_addr - sseg->start_ea;
    else
        sym.addr = -1;
}


////////////////////////////////////////////////////////////////////////////////
/// @brief Output a formatted string to messages window [analog of printf()]
///     only when the verbose flag of plugin's options is true
/// @param  format const char * printf() style message string.
/// @return void
/// @author TQN
/// @date 2004.09.11
 ////////////////////////////////////////////////////////////////////////////////
void showMsg(const char *format, ...)
{
    if (g_options.bVerbose)
    {
        va_list va;
        va_start(va, format);
        (void) vmsg(format, va);
        va_end(va);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Show options dialog for getting user desired options
/// @return void
/// @author TQN
/// @date 2004.09.11
////////////////////////////////////////////////////////////////////////////////
static void showOptionsDlg(void)
{
    // Build the format string constant used to create the dialog
    const char format[] =
        "STARTITEM 0\n"                             // TabStop
        "LoadMap Options\n"                         // Title
        "<Apply Map Symbols for Name:R>\n"          // Radio Button 0
        "<Apply Map Symbols for Comment:R>>\n"    // Radio Button 1
        "<Replace Existing Names/Comments:C>>\n"  // Checkbox Button
        "<Show verbose messages:C>>\n\n";           // Checkbox Button

    // Create the option dialog.
    short name = (g_options.bNameApply ? 0 : 1);
    short replace = (g_options.bReplace ? 1 : 0);
    short verbose = (g_options.bVerbose ? 1 : 0);
    if (ask_form(format, &name, &replace, &verbose))
    {
        g_options.bNameApply = (0 == name);
        g_options.bReplace = (1 == replace);
        g_options.bVerbose = (1 == verbose);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Write-side equivalent of read_config_file() from IDA API
/// @return True if saved
/// @author TL
/// @date 2023.11.22
 ////////////////////////////////////////////////////////////////////////////////
bool write_config_file(
        const char *filename,
        const cfgopt_t opts[],
        size_t nopts)
{
    char szLine[120];
    char szIniPath[MAXPATH] = { 0 };
    int fh, i;

    // Get the full path to user config dir
    qstrncpy(szIniPath, get_user_idadir(), sizeof(szIniPath));
    qstrncat(szIniPath, "/", sizeof(szIniPath));
    qstrncat(szIniPath, filename, sizeof(szIniPath));
    qstrncat(szIniPath, ".cfg", sizeof(szIniPath));
    szIniPath[sizeof(szIniPath) - 1] = '\0';

    fh = qcreate(szIniPath, 0644);
    if (fh == -1)
        return false;

    qsnprintf(szLine, sizeof(szLine), "//\n// LoadMap Plugin auto-saved configuration file\n//\n");
    qwrite(fh, szLine, qstrlen(szLine));

    // Write config in normal IDA format (like the files in IDA/cfg folder).
    // IDA Pro does not provide an API for that - only for reading.

    for (i = 0; i < nopts; i++)
    {
        const cfgopt_t *opt = &opts[i];

        qsnprintf(szLine, sizeof(szLine), "%s = %d\n", opt->name, *(int *)(opt->ptr));
        qwrite(fh, szLine, qstrlen(szLine));
    }
    qclose(fh);

    return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Plugin initialize function
/// @return PLUGIN_KEEP always
/// @author TQN
/// @date 2004.09.11
 ////////////////////////////////////////////////////////////////////////////////
static plugmod_t *idaapi init()
{
    msg("\nLoadMap: Plugin v%s init.\n\n", PLUG_VERSION);

    // Get options saved in cfg file; IDA Pro will find the file, it does
    // not need the full path nor extension, only base name.
    if (!read_config_file("loadmap", g_optsinfo, qnumber(g_optsinfo), NULL))
    {
        msg("LoadMap: Plugin config file '%s.cfg' read failed: %s.\n", "loadmap", qerrstr());
    }

#if IDA_SDK_VERSION >= 800
    switch (inf_get_filetype())
#else
    switch (inf.filetype)
#endif
    {
    case f_ZIP:
        return PLUGIN_SKIP;
    }
    return PLUGIN_KEEP;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Plugin run function, which does the actual job
/// @param   int    Not used
/// @return void
/// @author TQN
/// @date 2004.09.11
////////////////////////////////////////////////////////////////////////////////
bool idaapi run(size_t)
{
    static char mapFileName[_MAX_PATH] = { 0 };

    { // If user press shift key, show options dialog
#if 0
        // IDA API method - does not work in IDA 8/9, so disabling
        input_event_t input_event;
        if (get_user_input_event(&input_event) && (input_event.modifiers & VES_SHIFT))
#elif IDA_SDK_VERSION >= 800 && defined(HAS_IDA_QT_DEV_LIB)
        // Qt method - requires a special version of Qt which was used for building IDA
        Qt::KeyboardModifiers key = QApplication::queryKeyboardModifiers();
        if (key == Qt::ShiftModifier)
#else
        // Windows-only method
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
#endif
        {
            showOptionsDlg();
        }
    }

    unsigned long numOfSegs = get_segm_qty();
    if (0 == numOfSegs)
    {
        warning("Not found any segments");
        return false;
    }

    if ('\0' == mapFileName[0])
    {
        // First run (after all, mapFileName is static)
        get_input_file_path(mapFileName, sizeof(mapFileName));
        pathExtensionSwitch(mapFileName, ".map", sizeof(mapFileName));
    }

    // Show open map file dialog
    char *fname = ask_file(0, mapFileName, "Open MAP file");
    if (NULL == fname)
    {
        msg("LoadMap: User cancel\n");
        return false;
    }

    // Open the map file
    char * pMapStart = NULL;
    size_t mapSize = INVALID_MAPFILE_SIZE;
    MapFile::MAPResult eRet = MapFile::openMAP(fname, pMapStart, mapSize);
    switch (eRet)
    {
        case MapFile::WIN32_ERROR:
            warning("Could not open file '%s'.\nWin32 Error Code = 0x%08X",
                    fname, GetLastError());
            return false;

        case MapFile::FILE_EMPTY_ERROR:
            warning("File '%s' is empty, zero size", fname);
            return false;

        case MapFile::FILE_BINARY_ERROR:
            warning("File '%s' seem to be a binary or Unicode file", fname);
            return false;

        case MapFile::OPEN_NO_ERROR:
        default:
            break;
    }

    MapFile::SectionType sectnHdr = MapFile::NO_SECTION;
    unsigned long sectnNumber = 0;
    unsigned long validSyms = 0;
    unsigned long invalidSyms = 0;

    // The mark pointer to the end of memory map file
    // all below code must not read or write at and over it
    const char * pMapEnd = pMapStart + mapSize;

    show_wait_box("Parsing and applying symbols from the Map file '%s'", fname);

    try
    {
        const char * pLine = pMapStart;
        const char * pEOL = pMapStart;
        MapFile::MAPSymbol sym;
        MapFile::MAPSymbol prvsym;
        sym.seg = SREG_NUM;
        sym.addr = BADADDR;
        sym.name[0] = '\0';
        while (pLine < pMapEnd)
        {
            // Skip the spaces, '\r', '\n' characters, blank lines, seek to the
            // non space character at the beginning of a non blank line
            pLine = MapFile::skipSpaces(pEOL, pMapEnd);

            // Find the EOL '\r' or '\n' characters
            pEOL = MapFile::findEOL(pLine, pMapEnd);

            size_t lineLen = (size_t) (pEOL - pLine);
            if (lineLen < g_minLineLen)
            {
                continue;
            }
            char fmt[80];
            fmt[0] = '\0';

            // Check if we're on section header or section end
            if (sectnHdr == MapFile::NO_SECTION)
            {
                sectnHdr = MapFile::recognizeSectionStart(pLine, lineLen);
                if (sectnHdr != MapFile::NO_SECTION)
                {
                    sectnNumber++;
                    qsnprintf(fmt, sizeof(fmt), "Section start line: '%%.%ds'.\n", lineLen);
                    showMsg(fmt, pLine);
                    continue;
                }
            } else
            {
                sectnHdr = MapFile::recognizeSectionEnd(sectnHdr, pLine, lineLen);
                if (sectnHdr == MapFile::NO_SECTION)
                {
                    qsnprintf(fmt, sizeof(fmt), "Section end line: '%%.%ds'.\n", lineLen);
                    showMsg(fmt, pLine);
                    continue;
                }
            }
            MapFile::ParseResult parsed;
            prvsym.seg = sym.seg;
            prvsym.addr = sym.addr;
            qstrncpy(prvsym.name,sym.name,sizeof(sym.name));
            sym.seg = SREG_NUM;
            sym.addr = BADADDR;
            sym.name[0] = '\0';
            parsed = MapFile::INVALID_LINE;

            switch (sectnHdr)
            {
            case MapFile::NO_SECTION:
                parsed = MapFile::SKIP_LINE;
                break;
            case MapFile::MSVC_MAP:
            case MapFile::BCCL_NAM_MAP:
            case MapFile::BCCL_VAL_MAP:
                parsed = parseMsSymbolLine(sym,pLine,lineLen,g_minLineLen,numOfSegs);
                break;
            case MapFile::WATCOM_MAP:
                parsed = parseWatcomSymbolLine(sym,pLine,lineLen,g_minLineLen,numOfSegs);
                break;
            case MapFile::GCC_MAP:
                parsed = parseGccSymbolLine(sym,pLine,lineLen,g_minLineLen,numOfSegs);
                break;
            }

            if (parsed == MapFile::SKIP_LINE)
            {
                qsnprintf(fmt, sizeof(fmt), "Skipping line: '%%.%ds'.\n", lineLen);
                showMsg(fmt, pLine);
                continue;
            }
            if (parsed == MapFile::FINISHING_LINE)
            {
                sectnHdr = MapFile::NO_SECTION;
                // we have parsed to end of value/name symbols table or reached EOF
                qsnprintf(fmt, sizeof(fmt), "Parsing finished at line: '%%.%ds'.\n", lineLen);
                showMsg(fmt, pLine);
                continue;
            }
            if (parsed == MapFile::INVALID_LINE)
            {
                invalidSyms++;
                qsnprintf(fmt, sizeof(fmt), "Invalid map line: %%.%ds.\n", lineLen);
                showMsg(fmt, pLine);
                continue;
            }
            // If shouldn't apply names
            bool bNameApply = (g_options.bNameApply != 0);
            if (parsed == MapFile::COMMENT_LINE)
            {
                qsnprintf(fmt, sizeof(fmt), "Comment line: %%.%ds.\n", lineLen);
                showMsg(fmt, pLine);
                if (BADADDR == sym.addr)
                    continue;
            }
            // Determine the DeDe map file
            char *pname = sym.name;
            if (('<' == pname[0]) && ('-' == pname[1]))
            {
                // Functions indicator symbol of DeDe map
                pname += 2;
                bNameApply = true;
            }
            else if ('*' == pname[0])
            {
                // VCL controls indicator symbol of DeDe map
                pname++;
                bNameApply = false;
            }
            else if (('-' == pname[0]) && ('>' == pname[1]))
            {
                // VCL methods indicator symbol of DeDe map
                pname += 2;
                bNameApply = false;
            }

            ea_t la = sym.addr + getnseg((int) sym.seg)->start_ea;
            flags_t f = get_full_flags(la);

            bool didOk;
            if (bNameApply) // Apply symbols for name
            {
                //  Add name if there's no meaningful name assigned.
                if (g_options.bReplace ||
                    (!has_name(f) || has_dummy_name(f) || has_auto_name(f)))
                {
                    didOk = set_name(la, pname, SN_NOCHECK | SN_NOWARN);
#ifdef __EA64__
                    showMsg("%04lX:%08llX - Change name to '%s' %s\n",
                        sym.seg, la, pname, didOk ? "succeeded" : "failed");
#else
                    showMsg("%04lX:%08lX - Change name to '%s' %s\n",
                        sym.seg, la, pname, didOk ? "succeeded" : "failed");
#endif
                }
            }
            else if (g_options.bReplace || !has_cmt(f))
            {
                // Apply symbols for comment
                didOk = set_cmt(la, pname, false);
#ifdef __EA64__
                showMsg("%04lX:%08llX - Change comment to '%s' %s\n",
                    sym.seg, la, pname, didOk ? "succeeded" : "failed");
#else
                showMsg("%04lX:%08lX - Change comment to '%s' %s\n",
                    sym.seg, la, pname, didOk ? "succeeded" : "failed");
#endif
            }
            if (didOk)
                validSyms++;
            else
                invalidSyms++;
        }

    }
    catch (...)
    {
        warning("Exception while parsing MAP file '%s'");
        invalidSyms++;
    }
    MapFile::closeMAP(pMapStart);
    hide_wait_box();

    if (sectnNumber == 0)
    {
        warning("File '%s' is not a valid Map file; publics section header wasn't found", fname);
    }
    else
    {
        // Save file name for next askfile_c dialog
        qstrncpy(mapFileName, fname, sizeof(mapFileName));

        // Show the result
        msg("Result of loading and parsing the Map file '%s'\n"
            "   Number of Symbols applied: %d\n"
            "   Number of Invalid Symbols: %d\n\n",
            fname, validSyms, invalidSyms);
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief Plugin terminate callback function
/// @return void
/// @author TQN
/// @date 2004.09.11
////////////////////////////////////////////////////////////////////////////////
void idaapi term(void)
{
    msg("LoadMap: Plugin v%s terminate.\n", PLUG_VERSION);

    // Write the plugin's options to cfg file
    if (!write_config_file("loadmap", g_optsinfo, qnumber(g_optsinfo)))
    {
        msg("LoadMap: Plugin config file '%s.cfg' save failed: %s.\n", "loadmap", qerrstr());
    }
}

////////////////////////////////////////////////////////////////////////////////
/// @name Plugin information
/// @{
char wanted_name[]   = "Load Symbols From MAP File";
char wanted_hotkey[] = "Ctrl-M";
char comment[]       = "LoadMap loads symbols from a VC/BC/Watcom/Dede map file.";
char help[]          = "LoadMap " PLUG_VERSION ", Visual C/Borland C/Watcom C/Dede map file import plugin."
                              "This module reads selected map file, and loads symbols\n"
                              "into IDA database. Click it while holding Shift to see options.";
/// @}

////////////////////////////////////////////////////////////////////////////////
/// @brief Plugin description block
extern "C" {
plugin_t PLUGIN =
{
    IDP_INTERFACE_VERSION,
    0,                    // Plugin flags
    init,                 // Initialize
    term,                 // Terminate
    run,                  // Main function
    comment,              // Comment about the plugin
    help,
    wanted_name,          // preferred short name of the plugin
    wanted_hotkey         // preferred hotkey to run the plugin
};
};
////////////////////////////////////////////////////////////////////////////////
