/************************************************************************************* 
This file is a part of CrashRpt library.
Copyright (c) 2003-2013 The CrashRpt project authors. All Rights Reserved.

Use of this source code is governed by a BSD-style license
that can be found in the License.txt file in the root of the source
tree. All contributing project authors may
be found in the Authors.txt file in the root of the source tree.
***************************************************************************************/

// crashcon.cpp : Defines the entry point for the console application.
//

#include <stdio.h>
#include <conio.h>
#include <tchar.h>
#include <assert.h>
#include <process.h>
#include "CrashRpt.h" // Include CrashRpt header

LPVOID lpvState = NULL; // Not used, deprecated

BOOL WINAPI CrashCallback(LPVOID lpvState)
{
    UNREFERENCED_PARAMETER(lpvState);

    // Crash happened!

    return TRUE;
}


int main(int argc, char* argv[])
{
    argc; // this is to avoid C4100 unreferenced formal parameter warning
    argv; // this is to avoid C4100 unreferenced formal parameter warning

    // Install crash reporting

	CR_INSTALL_INFO info;
    memset(&info, 0, sizeof(CR_INSTALL_INFO));
    info.cb = sizeof(CR_INSTALL_INFO);  
    info.pszAppName = _T("ConsoleDemo"); // Define application name.
    info.pszAppVersion = _T("1.3.1");     // Define application version.
    //info.pszEmailSubject = _T("ConsoleDemo Error Report"); // Define subject for email.
    //info.pszEmailTo = _T("test@hotmail.com");   // Define E-mail recipient address.  
	//info.pszSmtpProxy = _T("127.0.0.1");  // Use SMTP proxy.
	//info.pszSmtpLogin = _T("test");      // SMTP Login
	//info.pszSmtpPassword = _T("test");       // SMTP Password
    info.pszUrl = _T("http://crashfix.local/index.php/crashReport/uploadExternal"); // URL for sending reports over HTTP.				
    info.pfnCrashCallback = CrashCallback; // Define crash callback function.   
    // Define delivery methods priorities. 
    info.uPriorities[CR_HTTP] = 3;         // Use HTTP the first.
   // info.uPriorities[CR_SMTP] = 2;         // Use SMTP the second.
    //info.uPriorities[CR_SMAPI] = 1;        // Use Simple MAPI the last.  
    info.dwFlags = 0;                    
    info.dwFlags |= CR_INST_ALL_POSSIBLE_HANDLERS; // Install all available exception handlers.    
    //info.dwFlags |= CR_INST_APP_RESTART;            // Restart the application on crash.  
    //info.dwFlags |= CR_INST_NO_MINIDUMP;          // Do not include minidump.
    info.dwFlags |= CR_INST_NO_GUI;               // Don't display GUI.
    //info.dwFlags |= CR_INST_DONT_SEND_REPORT;     // Don't send report immediately, just queue for later delivery.
    //info.dwFlags |= CR_INST_STORE_ZIP_ARCHIVES;   // Store ZIP archives along with uncompressed files (to be used with CR_INST_DONT_SEND_REPORT)
    //info.dwFlags |= CR_INST_SEND_MANDATORY;         // Remove "Close" and "Other actions..." buttons from Error Report dialog.
	//info.dwFlags |= CR_INST_SHOW_ADDITIONAL_INFO_FIELDS; //!< Make "Your E-mail" and "Describe what you were doing when the problem occurred" fields of Error Report dialog always visible.
	info.dwFlags |= CR_INST_ALLOW_ATTACH_MORE_FILES; //!< Adds an ability for user to attach more files to crash report by clicking "Attach More File(s)" item from context menu of Error Report Details dialog.
	//info.dwFlags |= CR_INST_SEND_QUEUED_REPORTS;    // Send reports that were failed to send recently.	
	//info.dwFlags |= CR_INST_AUTO_THREAD_HANDLERS; 
    info.pszDebugHelpDLL = NULL;                    // Search for dbghelp.dll using default search sequence.
    info.uMiniDumpType = MiniDumpNormal;            // Define minidump size.
    // Define privacy policy URL.
    //info.pszPrivacyPolicyURL = _T("http://code.google.com/p/crashrpt/wiki/PrivacyPolicyTemplate");
    //info.pszErrorReportSaveDir = NULL;       // Save error reports to the default location.
    //info.pszRestartCmdLine = _T("/restart"); // Command line for automatic app restart.
    //info.pszLangFilePath = _T("D:\\");       // Specify custom dir or filename for language file.
    //info.pszCustomSenderIcon = _T("C:\\WINDOWS\\System32\\user32.dll, 1"); // Specify custom icon for CrashRpt dialogs.
	//info.nRestartTimeout = 50;
    // Install crash handlers
    int nInstResult = crInstall(&info);            
    assert(nInstResult==0);

    // Check result
    if(nInstResult!=0)
    {
        TCHAR buff[256];
        crGetLastErrorMsg(buff, 256); // Get last error
        _tprintf(_T("%s\n"), buff); // and output it to the screen
        return FALSE;
    }

   // printf("Press Enter to simulate a null pointer exception or any other key to exit...\n");
    //int n = _getch();
   // if(n==13)
    {
        int *p = 0;
        *p = 0; // Access violation
    }

#ifdef TEST_DEPRECATED_FUNCS
    Uninstall(lpvState); // Uninstall exception handlers
#else
    int nUninstRes = crUninstall(); // Uninstall exception handlers
    assert(nUninstRes==0);
    nUninstRes;
#endif //TEST_DEPRECATED_FUNCS

    // Exit
    return 0;
}

