/*
 * Copyright (C) 1994-1998 T. Teranishi
 * (C) 2007-2018 TeraTerm Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* TERATERM.EXE, TTDLG interface */
#include "teraterm.h"
#include "tttypes.h"
#include "ttplug.h" /* TTPLUG */
#include "ttdlg.h"

#include "ttdialog.h"

PSetupTerminal SetupTerminal = _SetupTerminal;
PSetupWin SetupWin = _SetupWin;
PSetupKeyboard SetupKeyboard = _SetupKeyboard;
PSetupSerialPort SetupSerialPort = _SetupSerialPort;
PSetupTCPIP SetupTCPIP = _SetupTCPIP;
PGetHostName GetHostName = _GetHostName;
PChangeDirectory ChangeDirectory = _ChangeDirectory;
PAboutDialog AboutDialog = _AboutDialog;
PChooseFontDlg ChooseFontDlg = _ChooseFontDlg;
PSetupGeneral SetupGeneral = _SetupGeneral;
PWindowWindow WindowWindow = _WindowWindow;
PTTDLGSetUILanguageFile TTDLGSetUILanguageFile = _TTDLGSetUILanguageFile;

BOOL LoadTTDLG()
{
	static BOOL initialized;
	if (!initialized) {
		TTXGetUIHooks(); /* TTPLUG */
		initialized = TRUE;
	}
	return TRUE;
}

BOOL FreeTTDLG()
{
	return TRUE;
}
