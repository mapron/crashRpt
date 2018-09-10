/************************************************************************************* 
This file is a part of CrashRpt library.
Copyright (c) 2003-2013 The CrashRpt project authors. All Rights Reserved.

Use of this source code is governed by a BSD-style license
that can be found in the License.txt file in the root of the source
tree. All contributing project authors may
be found in the Authors.txt file in the root of the source tree.
***************************************************************************************/

#include "commonInclude.h"
#include "ErrorReportSender.h"
#include "CrashRpt.h"
#include "md5.h"
#include "Utility.h"
#include "zip.h"
#include "CrashInfoReader.h"
#include "strconv.h"
#include "base64.h"
#include "tinyxml.h"
#include <sys/stat.h>
#include "dbghelp.h"

CErrorReportSender* CErrorReportSender::m_pInstance = NULL;

// Constructor
CErrorReportSender::CErrorReportSender()
{
    // Init variables
    m_nStatus = 0;
    m_nCurReport = 0;
    m_hThread = NULL;
    m_SendAttempt = 0;
    m_Action=COLLECT_CRASH_INFO;
    m_bExport = FALSE;
	m_MailClientConfirm = NOT_CONFIRMED_YET;	
    m_bSendingNow = FALSE;        
	m_bErrors = FALSE;
}

// Destructor
CErrorReportSender::~CErrorReportSender()
{
	Finalize();
}

CErrorReportSender* CErrorReportSender::GetInstance()
{
	// Return singleton object
	if(m_pInstance==NULL)	
		m_pInstance = new CErrorReportSender();
	return m_pInstance;
}

BOOL CErrorReportSender::Init(LPCTSTR szFileMappingName)
{
	m_sErrorMsg = _T("Unspecified error.");
		
	// Read crash information from the file mapping object.
    int nInit = m_CrashInfo.Init(szFileMappingName);
    if(nInit!=0)
    {
		m_sErrorMsg.Format(_T("Error reading crash info: %s"), m_CrashInfo.GetErrorMsg().GetBuffer(0));
        return FALSE;
    }
		
	// Check window mirroring settings 
    /*CString sRTL = Utility::GetINIString(m_CrashInfo.m_sLangFileName, _T("Settings"), _T("RTLReading"));
    if(sRTL.CompareNoCase(_T("1"))==0)
    {
		// Set Right-to-Left reading order
        SetProcessDefaultLayout(LAYOUT_RTL);  
    }*/
	
    if(!m_CrashInfo.m_bSendRecentReports)
    {
        // Start crash info collection work assynchronously
        DoWorkAssync(COLLECT_CRASH_INFO);
    }
	else
	{
		// Check if another instance of CrashSender.exe is running.
        ::CreateMutex( NULL, FALSE,_T("Local\\43773530-129a-4298-88f2-20eea3e4a59b"));
        if (::GetLastError() == ERROR_ALREADY_EXISTS)
        {		
            m_sErrorMsg = _T("Another CrashSender.exe already tries to resend recent reports.");
            return FALSE;
        }

        if(m_CrashInfo.GetReportCount()==0)
		{
			m_sErrorMsg = _T("There are no reports for us to send.");
            return FALSE; 
		}

        // Check if it is ok to remind user now.
        if(!m_CrashInfo.IsRemindNowOK())
		{
			m_sErrorMsg = _T("Not enough time elapsed to remind user about recent crash reports.");
            return FALSE;
		}
	}

	// Done.
	m_sErrorMsg = _T("Success.");
	return TRUE;
}

BOOL CErrorReportSender::InitLog()
{
	// Check if we have already created log
	if(!m_sCrashLogFile.IsEmpty())
		return TRUE;

	// Create directory where we will store recent crash logs
	CString sLogDir = m_CrashInfo.m_sUnsentCrashReportsFolder + _T("\\Logs");	
	BOOL bCreateDir = Utility::CreateFolder(sLogDir);
    if(!bCreateDir)
    {        
        return FALSE; 
    }

	// Remove crash logs above the maximum allowed count
	const int MAX_CRASH_LOG_COUNT = 10;
	WTL::CFindFile FileFinder;
	std::set<CString> asLogFiles;

	BOOL bFound = FileFinder.FindFile(sLogDir + _T("\\*.txt"));
	while(bFound)
	{
		CString sFile = FileFinder.GetFileName();
		if(FileFinder.IsDots() || FileFinder.IsDirectory())
			continue; // Skip ".", ".." and dirs
	
		// Add log to list
		asLogFiles.insert(FileFinder.GetFilePath());

		// Find next log
		bFound = FileFinder.FindNextFile();
	}

	while(asLogFiles.size()>=MAX_CRASH_LOG_COUNT)
	{
		std::set<CString>::iterator it = asLogFiles.begin();
		CString sLogFile = *it;
		asLogFiles.erase(it);
		
		Utility::RecycleFile(sLogFile, TRUE);
	}
	
	// Create new log file
	time_t cur_time;
    time(&cur_time);
    TCHAR szDateTime[64];

    struct tm timeinfo;
    localtime_s(&timeinfo, &cur_time);
    _tcsftime(szDateTime, 64,  _T("%Y%m%d-%H%M%S"), &timeinfo);
	
	CString sLogFile;
	sLogFile.Format(_T("%s\\CrashRpt-Log-%s-{%s}.txt"), 
		sLogDir, szDateTime, 
		m_CrashInfo.m_bSendRecentReports?_T("batch"):m_CrashInfo.GetReport(0)->GetCrashGUID());
	m_Assync.InitLogFile(sLogFile);
	
	m_sCrashLogFile = sLogFile;

	return TRUE;
}

CCrashInfoReader* CErrorReportSender::GetCrashInfo()
{
	return &m_CrashInfo;
}

CString CErrorReportSender::GetErrorMsg()
{
	return m_sErrorMsg;
}

void CErrorReportSender::SetNotificationWindow(HWND hWnd)
{
	// Set notification window
	m_hWndNotify = hWnd;
}

BOOL CErrorReportSender::Run()
{
	if(m_CrashInfo.m_bSendRecentReports)
	{
		// We should send recently queued error reports.
		DoWorkAssync(SEND_RECENT_REPORTS);    
	}
	else
	{
		// Wait for completion of crash info collector.
		WaitForCompletion();

		// Determine whether to send crash report now 
		// or to exit without sending report.
		if(m_CrashInfo.m_bSendErrorReport) // If we should send error report now
		{
			// Compress report files and send the report
			SetExportFlag(FALSE, _T(""));
			DoWorkAssync(COMPRESS_REPORT|RESTART_APP|SEND_REPORT);    
		}
		else // If we shouldn't send error report now
		{      
			// Exit        

		}
	}

	return TRUE;
}

int CErrorReportSender::GetStatus()
{
    // Return global error report delivery status
    return m_nStatus;
}

int CErrorReportSender::GetCurReport()
{
    // Returns the index of error report currently being sent
    return m_nCurReport;
}

// This method performs crash files collection and/or
// error report sending work in a worker thread.
BOOL CErrorReportSender::DoWorkAssync(int nAction)
{
    // Save the action code
    m_Action = nAction;

    // Create worker thread which will do all work assynchronously
    m_hThread = CreateThread(NULL, 0, WorkerThread, (LPVOID)this, 0, NULL);

    // Check if the thread was created ok
    if(m_hThread==NULL)
        return FALSE; // Failed to create worker thread

    // Done, return
    return TRUE;
}

// This method is the worker thread procedure that delegates further work 
// back to the CErrorReportSender class
DWORD WINAPI CErrorReportSender::WorkerThread(LPVOID lpParam)
{
    // Delegate the action to the CErrorReportSender::DoWorkAssync() method
    CErrorReportSender* pSender = (CErrorReportSender*)lpParam;
    pSender->DoWork(pSender->m_Action);
	pSender->m_hThread = NULL; // clean up
    // Exit code can be ignored
    return 0;
}

// This method unblocks the parent process
void CErrorReportSender::UnblockParentProcess()
{
    // Notify the parent process that we have finished with minidump,
    // so the parent process is able to unblock and terminate itself.

	// Open the event the parent process had created for us
    CString sEventName;
    sEventName.Format(_T("Local\\CrashRptEvent_%s"), GetCrashInfo()->GetReport(0)->GetCrashGUID());
    HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, sEventName);
    if(hEvent!=NULL)
        SetEvent(hEvent); // Signal event
}

// This method collects required crash report files (minidump, screenshot etc.)
// and then sends the error report over the Internet.
BOOL CErrorReportSender::DoWork(int Action)
{
    // Reset the completion event
    m_Assync.Reset();

	// Init crash log
	InitLog();

    if(Action&SEND_RECENT_REPORTS) // If we are currently sending pending error reports
    {
		return SendRecentReports();		        
    }

    if(Action&COLLECT_CRASH_INFO) // Collect crash report files
    {
        // Add a message to log
        m_Assync.SetProgress(_T("Start collecting information about the crash..."), 0, false);
       
        if(m_Assync.IsCancelled()) // Check if user-cancelled
        {      
            // Parent process can now terminate
            UnblockParentProcess();

            // Add a message to log
            m_Assync.SetProgress(_T("[exit_silently]"), 0, false);
            return FALSE;
        }

        // Create crash dump.
        CreateMiniDump();

        if(m_Assync.IsCancelled()) // Check if user-cancelled
        {      
            // Parent process can now terminate
            UnblockParentProcess();

            // Add a message to log
            m_Assync.SetProgress(_T("[exit_silently]"), 0, false);
            return FALSE;
        }

        // Notify the parent process that we have finished with minidump,
        // so the parent process is able to unblock and terminate itself.
        UnblockParentProcess();
		
        // Copy user-provided files.
        CollectCrashFiles();

        if(m_Assync.IsCancelled()) // Check if user-cancelled
        {      
            // Add a message to log
            m_Assync.SetProgress(_T("[exit_silently]"), 0, false);
            return FALSE;
        }
		
		// Create crash description XML
		CreateCrashDescriptionXML(*m_CrashInfo.GetReport(0));
	
		// Add a message to log
        m_Assync.SetProgress(_T("[confirm_send_report]"), 100, false);
    }

	if(Action&RESTART_APP) // We need to restart the parent process
    { 
        // Restart the application
        RestartApp();
    }

    if(Action&COMPRESS_REPORT) // We have to compress error report file into ZIP archive
    { 
        // Compress error report files
        BOOL bCompress = CompressReportFiles(m_CrashInfo.GetReport(m_nCurReport));
        if(!bCompress)
        {
            // Add a message to log
            m_Assync.SetProgress(_T("[status_failed]"), 100, false);
            return FALSE; // Error compressing files
        }
    }
	
    if(Action&SEND_REPORT) // We need to send the report over the Internet
    {
        // Send the error report.
        if(!SendReport())
			return FALSE;
    }

    // Done
    return TRUE;
}

// Returns the export flag (the flag is set if we are exporting error report as a ZIP archive)
void CErrorReportSender::SetExportFlag(BOOL bExport, CString sExportFile)
{
    // This is used when we need to export error report files as a ZIP archive
    m_bExport = bExport;
    m_sExportFileName = sExportFile;
}

// This method blocks until worker thread is exited
void CErrorReportSender::WaitForCompletion()
{	
	if(m_hThread!=NULL)
		WaitForSingleObject(m_hThread, INFINITE);	
}

// Gets status of the local operation
void CErrorReportSender::GetCurOpStatus(int& nProgressPct, std::vector<CString>& msg_log)
{
    m_Assync.GetProgress(nProgressPct, msg_log); 
}

// This method cancels the current operation
void CErrorReportSender::Cancel()
{
    // User-cancelled
    m_Assync.Cancel();
}

// This method notifies the main thread that we have finished assync operation
void CErrorReportSender::FeedbackReady(int code)
{
    m_Assync.FeedbackReady(code);
}

// This method cleans up temporary files
BOOL CErrorReportSender::Finalize()
{  
	// Wait until worker thread exits.
	WaitForCompletion();

    if((m_CrashInfo.m_bSendErrorReport && !m_CrashInfo.m_bQueueEnabled) ||
		(m_CrashInfo.m_bAddVideo && !m_CrashInfo.m_bClientAppCrashed))
    {
        // Remove report files if queue disabled (or if client app not crashed).
        Utility::RecycleFile(m_CrashInfo.GetReport(0)->GetErrorReportDirName(), true);    
    }
		
    if(!m_CrashInfo.m_bSendErrorReport && 
        m_CrashInfo.m_bStoreZIPArchives) // If we should generate a ZIP archive
    {
        // Compress error report files
        DoWork(COMPRESS_REPORT);            
    }

    // If needed, restart the application
    if(!m_CrashInfo.m_bSendRecentReports)
    {
        DoWork(RESTART_APP);         
    }

    // Done OK
    return TRUE;
}

// This callback function is called by MinidumpWriteDump
BOOL CALLBACK CErrorReportSender::MiniDumpCallback(
    PVOID CallbackParam,
    PMINIDUMP_CALLBACK_INPUT CallbackInput,
    PMINIDUMP_CALLBACK_OUTPUT CallbackOutput )
{
    // Delegate back to the CErrorReportSender
    CErrorReportSender* pErrorReportSender = (CErrorReportSender*)CallbackParam;  
    return pErrorReportSender->OnMinidumpProgress(CallbackInput, CallbackOutput);  
}

// This method is called when MinidumpWriteDump notifies us about
// currently performed action
BOOL CErrorReportSender::OnMinidumpProgress(const PMINIDUMP_CALLBACK_INPUT CallbackInput,
                                            PMINIDUMP_CALLBACK_OUTPUT CallbackOutput)
{
    switch(CallbackInput->CallbackType)
    {
    case CancelCallback: 
        {
            // This callback allows to cancel minidump generation
            if(m_Assync.IsCancelled())
            {
                CallbackOutput->Cancel = TRUE;      
                m_Assync.SetProgress(_T("Dump generation cancelled by user"), 0, true);
            }
        }
        break;

    case ModuleCallback:
        {
            // We are currently dumping some module
            strconv_t strconv;
            CString sMsg;
            sMsg.Format(_T("Dumping info for module %s"), 
                strconv.w2t(CallbackInput->Module.FullPath));

			// Here we want to collect module information
			CErrorReportInfo* eri = m_CrashInfo.GetReport(0);
			if(eri->GetExceptionAddress()!=0)
			{
				// Check if this is the module where exception has happened
				ULONG64 dwExcAddr = eri->GetExceptionAddress();
				if(dwExcAddr>=CallbackInput->Module.BaseOfImage && 
					dwExcAddr<=CallbackInput->Module.BaseOfImage+CallbackInput->Module.SizeOfImage)
				{
					// Save module information to the report
					eri->SetExceptionModule(CallbackInput->Module.FullPath);
					eri->SetExceptionModuleBase(CallbackInput->Module.BaseOfImage);

					// Save module version info
					VS_FIXEDFILEINFO* fi = &CallbackInput->Module.VersionInfo;
					if(fi)
					{
						WORD dwVerMajor = HIWORD(fi->dwProductVersionMS);
						WORD dwVerMinor = LOWORD(fi->dwProductVersionMS);
						WORD dwPatchLevel = HIWORD(fi->dwProductVersionLS);
						WORD dwVerBuild = LOWORD(fi->dwProductVersionLS);

						CString sVer;
						sVer.Format(_T("%u.%u.%u.%u"), 
									dwVerMajor, dwVerMinor, dwPatchLevel, dwVerBuild);
						eri->SetExceptionModuleVersion(sVer);  					
					}
				}
			}

			// Update progress
            m_Assync.SetProgress(sMsg, 0, true);
        }
        break;
    case ThreadCallback:
        {      
            // We are currently dumping some thread 
            CString sMsg;
            sMsg.Format(_T("Dumping info for thread 0x%X"), 
                CallbackInput->Thread.ThreadId);
            m_Assync.SetProgress(sMsg, 0, true);
        }
        break;

    }

    return TRUE;
}

// This method creates the minidump of the process
BOOL CErrorReportSender::CreateMiniDump()
{   
    BOOL bStatus = FALSE;
    HMODULE hDbgHelp = NULL;
    HANDLE hFile = NULL;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    MINIDUMP_CALLBACK_INFORMATION mci;
    CString sMinidumpFile = m_CrashInfo.GetReport(m_nCurReport)->
        GetErrorReportDirName() + _T("\\crashdump.dmp");
    std::vector<ERIFileItem> files_to_add;
    ERIFileItem fi;
    CString sErrorMsg;

	// Check our config - should we generate the minidump or not?
    if(m_CrashInfo.m_bGenerateMinidump==FALSE)
    {
        m_Assync.SetProgress(_T("Crash dump generation disabled; skipping."), 0, false);
        return TRUE;
    }

	// Update progress
    m_Assync.SetProgress(_T("Creating crash dump file..."), 0, false);
    m_Assync.SetProgress(_T("[creating_dump]"), 0, false);
	
    // Load dbghelp.dll
    hDbgHelp = LoadLibrary(m_CrashInfo.m_sDbgHelpPath);
    if(hDbgHelp==NULL)
    {
        // Try again ... fallback to dbghelp.dll in path
        const CString sDebugHelpDLL_name = "dbghelp.dll";
        hDbgHelp = LoadLibrary(sDebugHelpDLL_name);    
    }

    if(hDbgHelp==NULL)
    {
        sErrorMsg = _T("dbghelp.dll couldn't be loaded");
        m_Assync.SetProgress(_T("dbghelp.dll couldn't be loaded."), 0, false);
        goto cleanup;
    }

	// Try to adjust process privilegies to be able to generate minidumps.
	SetDumpPrivileges();

    // Create the minidump file
    hFile = CreateFile(
        sMinidumpFile,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

	// Check if file has been created
    if(hFile==INVALID_HANDLE_VALUE)
    {
        DWORD dwError = GetLastError();
        CString sMsg;    
        sMsg.Format(_T("Couldn't create minidump file: %s"), 
            Utility::FormatErrorMsg(dwError));
        m_Assync.SetProgress(sMsg, 0, false);
        sErrorMsg = sMsg;
        return FALSE;
    }

    // Set valid dbghelp API version  
    typedef LPAPI_VERSION (WINAPI* LPIMAGEHLPAPIVERSIONEX)(LPAPI_VERSION AppVersion);  
    LPIMAGEHLPAPIVERSIONEX lpImagehlpApiVersionEx = 
        (LPIMAGEHLPAPIVERSIONEX)GetProcAddress(hDbgHelp, "ImagehlpApiVersionEx");
    ATLASSERT(lpImagehlpApiVersionEx!=NULL);
    if(lpImagehlpApiVersionEx!=NULL)
    {    
        API_VERSION CompiledApiVer;
        CompiledApiVer.MajorVersion = 6;
        CompiledApiVer.MinorVersion = 1;
        CompiledApiVer.Revision = 11;    
        CompiledApiVer.Reserved = 0;
        LPAPI_VERSION pActualApiVer = lpImagehlpApiVersionEx(&CompiledApiVer);    
        pActualApiVer;
		//_ASSERT_EXPR(CompiledApiVer.MinorVersion==pActualApiVer->MinorVersion, std::to_wstring(pActualApiVer->MinorVersion).c_str());
        ATLASSERT(CompiledApiVer.MajorVersion<=pActualApiVer->MajorVersion);
        //ATLASSERT(CompiledApiVer.MinorVersion==pActualApiVer->MinorVersion);
        //ATLASSERT(CompiledApiVer.Revision==pActualApiVer->Revision);    
    }

    // Write minidump to the file
    mei.ThreadId = m_CrashInfo.m_dwThreadId;
    mei.ExceptionPointers = m_CrashInfo.m_pExInfo;
    mei.ClientPointers = TRUE;

    mci.CallbackRoutine = MiniDumpCallback;
    mci.CallbackParam = this;

    typedef BOOL (WINAPI *LPMINIDUMPWRITEDUMP)(
        HANDLE hProcess, 
        DWORD ProcessId, 
        HANDLE hFile, 
        MINIDUMP_TYPE DumpType, 
        CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam, 
        CONST PMINIDUMP_USER_STREAM_INFORMATION UserEncoderParam, 
        CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam);

	// Get address of MiniDumpWirteDump function
    LPMINIDUMPWRITEDUMP pfnMiniDumpWriteDump = 
        (LPMINIDUMPWRITEDUMP)GetProcAddress(hDbgHelp, "MiniDumpWriteDump");
    if(!pfnMiniDumpWriteDump)
    {    
        m_Assync.SetProgress(_T("Bad MiniDumpWriteDump function."), 0, false);
        sErrorMsg = _T("Bad MiniDumpWriteDump function");
        return FALSE;
    }

	// Open client process
    HANDLE hProcess = OpenProcess(
        PROCESS_ALL_ACCESS, 
        FALSE, 
        m_CrashInfo.m_dwProcessId);

	// Now actually write the minidump
    BOOL bWriteDump = pfnMiniDumpWriteDump(
        hProcess,
        m_CrashInfo.m_dwProcessId,
        hFile,
        m_CrashInfo.m_MinidumpType,
        &mei,
        NULL,
        &mci);

	// Check result
    if(!bWriteDump)
    {    
        CString sMsg = Utility::FormatErrorMsg(GetLastError());
        m_Assync.SetProgress(_T("Error writing dump."), 0, false);
        m_Assync.SetProgress(sMsg, 0, false);
        sErrorMsg = sMsg;
        goto cleanup;
    }

	// Update progress
    bStatus = TRUE;
    m_Assync.SetProgress(_T("Finished creating dump."), 100, false);

cleanup:

    // Close file
    if(hFile)
        CloseHandle(hFile);

    // Unload dbghelp.dll
    if(hDbgHelp)
        FreeLibrary(hDbgHelp);

	// Add the minidump file to error report
    fi.m_bMakeCopy = false;
    fi.m_sDesc = _T("DescCrashDump");
    fi.m_sDestFile = _T("crashdump.dmp");
    fi.m_sSrcFile = sMinidumpFile;
    fi.m_sErrorStatus = sErrorMsg;
    files_to_add.push_back(fi);

    // Add file to the list
    m_CrashInfo.GetReport(0)->AddFileItem(&fi);
	
    return bStatus;
}

BOOL CErrorReportSender::SetDumpPrivileges()
{
	// This method is used to have the current process be able to call MiniDumpWriteDump
	// This code was taken from:
	// http://social.msdn.microsoft.com/Forums/en-US/vcgeneral/thread/f54658a4-65d2-4196-8543-7e71f3ece4b6/
	

	BOOL       fSuccess  = FALSE;
	HANDLE      TokenHandle = NULL;
	TOKEN_PRIVILEGES TokenPrivileges;

	if (!OpenProcessToken(GetCurrentProcess(),
		TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
		&TokenHandle))
	{
		m_Assync.SetProgress(_T("SetDumpPrivileges: Could not get the process token"), 0);		
		goto Cleanup;
	}

	TokenPrivileges.PrivilegeCount = 1;

	if (!LookupPrivilegeValue(NULL,
		SE_DEBUG_NAME,
		&TokenPrivileges.Privileges[0].Luid))
	{
		m_Assync.SetProgress(_T("SetDumpPrivileges: Couldn't lookup SeDebugPrivilege name"), 0);				
		goto Cleanup;
	}

	TokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	//Add privileges here.
	if (!AdjustTokenPrivileges(TokenHandle,
		FALSE,
		&TokenPrivileges,
		sizeof(TokenPrivileges),
		NULL,
		NULL))
	{
		m_Assync.SetProgress(_T("SetDumpPrivileges: Could not revoke the debug privilege"), 0);						
		goto Cleanup;
	}

	fSuccess = TRUE;

Cleanup:

	if (TokenHandle)
	{
		CloseHandle(TokenHandle);
	}

	return fSuccess;
}

// This method adds an element to XML file
void CErrorReportSender::AddElemToXML(CString sName, CString sValue, TiXmlNode* root)
{
    strconv_t strconv;
    TiXmlHandle hElem = new TiXmlElement(strconv.t2utf8(sName));
    root->LinkEndChild(hElem.ToNode());
    TiXmlText* text = new TiXmlText(strconv.t2utf8(sValue));
    hElem.ToElement()->LinkEndChild(text);
}

// This method generates an XML file describing the crash
BOOL CErrorReportSender::CreateCrashDescriptionXML(CErrorReportInfo& eri)
{
    BOOL bStatus = FALSE;
    ERIFileItem fi;
    CString sFileName = eri.GetErrorReportDirName() + _T("\\crashrpt.xml");
    CString sErrorMsg;
    strconv_t strconv;
    TiXmlDocument doc;
    FILE* f = NULL; 
    CString sNum;
    CString sCrashRptVer;
    CString sOSIs64Bit;
    CString sExceptionType;

    fi.m_bMakeCopy = false;
    fi.m_sDesc = _T("DescXML");
    fi.m_sDestFile = _T("crashrpt.xml");
    fi.m_sSrcFile = sFileName;
    fi.m_sErrorStatus = sErrorMsg;  
    // Add this file to the list
    eri.AddFileItem(&fi);

    TiXmlNode* root = root = new TiXmlElement("CrashRpt");
    doc.LinkEndChild(root);  
    sCrashRptVer.Format(_T("%d"), CRASHRPT_VER);
    TiXmlHandle(root).ToElement()->SetAttribute("version", strconv.t2utf8(sCrashRptVer));

    TiXmlDeclaration * decl = new TiXmlDeclaration( "1.0", "UTF-8", "" );
    doc.InsertBeforeChild(root, *decl);
	
    AddElemToXML(_T("CrashGUID"), eri.GetCrashGUID(), root);
    AddElemToXML(_T("AppName"), eri.GetAppName(), root);
    AddElemToXML(_T("AppVersion"), eri.GetAppVersion(), root);  
    AddElemToXML(_T("ImageName"), eri.GetImageName(), root);
    AddElemToXML(_T("OperatingSystem"), eri.GetOSName(), root);


    sOSIs64Bit.Format(_T("%d"), eri.IsOS64Bit());
    AddElemToXML(_T("OSIs64Bit"), sOSIs64Bit, root);

    AddElemToXML(_T("GeoLocation"), eri.GetGeoLocation(), root);
    AddElemToXML(_T("SystemTimeUTC"), eri.GetSystemTimeUTC(), root);
		
	if(eri.GetExceptionAddress()!=0)
	{
		sNum.Format(_T("0x%I64x"), eri.GetExceptionAddress());
		AddElemToXML(_T("ExceptionAddress"), sNum, root);

		AddElemToXML(_T("ExceptionModule"), eri.GetExceptionModule(), root);

		sNum.Format(_T("0x%I64x"), eri.GetExceptionModuleBase());
		AddElemToXML(_T("ExceptionModuleBase"), sNum, root);

		AddElemToXML(_T("ExceptionModuleVersion"), eri.GetExceptionModuleVersion(), root);
	}

    sExceptionType.Format(_T("%d"), m_CrashInfo.m_nExceptionType);
    AddElemToXML(_T("ExceptionType"), sExceptionType, root);
    if(m_CrashInfo.m_nExceptionType==CR_SEH_EXCEPTION)
    {
        CString sExceptionCode;
        sExceptionCode.Format(_T("%d"), m_CrashInfo.m_dwExceptionCode);
        AddElemToXML(_T("ExceptionCode"), sExceptionCode, root);
    }
    else if(m_CrashInfo.m_nExceptionType==CR_CPP_SIGFPE)
    {
        CString sFPESubcode;
        sFPESubcode.Format(_T("%d"), m_CrashInfo.m_uFPESubcode);
        AddElemToXML(_T("FPESubcode"), sFPESubcode, root);
    }
    else if(m_CrashInfo.m_nExceptionType==CR_CPP_INVALID_PARAMETER)
    {
        AddElemToXML(_T("InvParamExpression"), m_CrashInfo.m_sInvParamExpr, root);
        AddElemToXML(_T("InvParamFunction"), m_CrashInfo.m_sInvParamFunction, root);
        AddElemToXML(_T("InvParamFile"), m_CrashInfo.m_sInvParamFile, root);

        CString sInvParamLine;
        sInvParamLine.Format(_T("%d"), m_CrashInfo.m_uInvParamLine);
        AddElemToXML(_T("InvParamLine"), sInvParamLine, root);
    }

    CString sGuiResources;
    sGuiResources.Format(_T("%d"), eri.GetGuiResourceCount());
    AddElemToXML(_T("GUIResourceCount"), sGuiResources, root);

    CString sProcessHandleCount;
    sProcessHandleCount.Format(_T("%d"), eri.GetProcessHandleCount());
    AddElemToXML(_T("OpenHandleCount"), sProcessHandleCount, root);

    AddElemToXML(_T("MemoryUsageKbytes"), eri.GetMemUsage(), root);

	TiXmlHandle hCustomProps = new TiXmlElement("CustomProps");
    root->LinkEndChild(hCustomProps.ToNode());

	int i;
	for(i=0; i<eri.GetPropCount(); i++)
    { 
		CString sName;
		CString sVal;
		eri.GetPropByIndex(i, sName, sVal);

        TiXmlHandle hProp = new TiXmlElement("Prop");

        hProp.ToElement()->SetAttribute("name", strconv.t2utf8(sName));
        hProp.ToElement()->SetAttribute("value", strconv.t2utf8(sVal));

        hCustomProps.ToElement()->LinkEndChild(hProp.ToNode());                  
    }

    TiXmlHandle hFileItems = new TiXmlElement("FileList");
    root->LinkEndChild(hFileItems.ToNode());

    for(i=0; i<eri.GetFileItemCount(); i++)
    {    
		ERIFileItem* rfi = eri.GetFileItemByIndex(i);
        TiXmlHandle hFileItem = new TiXmlElement("FileItem");

        hFileItem.ToElement()->SetAttribute("name", strconv.t2utf8(rfi->m_sDestFile));
        hFileItem.ToElement()->SetAttribute("description", strconv.t2utf8(rfi->m_sDesc));
		if(rfi->m_bAllowDelete)
			hFileItem.ToElement()->SetAttribute("optional", "1");
        if(!rfi->m_sErrorStatus.IsEmpty())
            hFileItem.ToElement()->SetAttribute("error", strconv.t2utf8(rfi->m_sErrorStatus));

        hFileItems.ToElement()->LinkEndChild(hFileItem.ToNode());                  
    }

#if _MSC_VER<1400
    f = _tfopen(sFileName, _T("w"));
#else
    _tfopen_s(&f, sFileName, _T("w"));
#endif

    if(f==NULL)
    {
        sErrorMsg = _T("Error opening file for writing");
        goto cleanup;
    }

    doc.useMicrosoftBOM = true;
    bool bSave = doc.SaveFile(f); 
    if(!bSave)
    {
        sErrorMsg = doc.ErrorDesc();
        goto cleanup;
    }

    fclose(f);
    f = NULL;

    bStatus = TRUE;

cleanup:

    if(f)
        fclose(f);

    if(!bStatus)
    {
		eri.GetFileItemByName(fi.m_sDestFile)->m_sErrorStatus = sErrorMsg;
    }

    return bStatus;
}

// This method collects user-specified files
BOOL CErrorReportSender::CollectCrashFiles()
{ 
	BOOL bStatus = FALSE;
    CString str;
    CString sErrorReportDir = m_CrashInfo.GetReport(m_nCurReport)->GetErrorReportDirName();
    CString sSrcFile;
    CString sDestFile;    
	std::vector<ERIFileItem> file_list;
	
    // Copy application-defined files that should be copied on crash
    m_Assync.SetProgress(_T("[copying_files]"), 0, false);
	
	// Walk through error report files
    int i;
	for(i=0; i<m_CrashInfo.GetReport(m_nCurReport)->GetFileItemCount(); i++)
    {
		ERIFileItem* pfi = m_CrashInfo.GetReport(m_nCurReport)->GetFileItemByIndex(i);

		// Check if operation has been cancelled by user
        if(m_Assync.IsCancelled())
            goto cleanup;

		// Check if the file name is a search template.		
		BOOL bSearchPattern = Utility::IsFileSearchPattern(pfi->m_sSrcFile);
		if(bSearchPattern)
			CollectFilesBySearchTemplate(pfi, file_list);
		else
			CollectSingleFile(pfi);
    }

	// Add newly collected files to the list of file items
	for(i=0; i<(int)file_list.size(); i++)
	{
		m_CrashInfo.GetReport(0)->AddFileItem(&file_list[i]);
	}	
	for (const auto & filename : m_additionalFiles)
	{
		ERIFileItem fi;
        fi.m_sSrcFile = filename.fullpath.c_str();
		fi.m_sDestFile = filename.shortname.c_str();
        fi.m_sDesc = filename.shortname.c_str();
		m_CrashInfo.GetReport(0)->AddFileItem(&fi);
	}

	// Remove file items that are search patterns
	BOOL bFound = FALSE;
	do
	{
		bFound = FALSE;
		for(i=0; i<m_CrashInfo.GetReport(m_nCurReport)->GetFileItemCount(); i++)
		{
			ERIFileItem* pfi = m_CrashInfo.GetReport(m_nCurReport)->GetFileItemByIndex(i);
				
			// Check if the file name is a search template.		
			BOOL bSearchPattern = Utility::IsFileSearchPattern(pfi->m_sSrcFile);
			if(bSearchPattern)
			{
				// Delete this item
				m_CrashInfo.GetReport(m_nCurReport)->DeleteFileItemByIndex(i);
				bFound = TRUE;
				break;
			}
		}
	}
	while(bFound);

    // Create dump of registry keys

    CErrorReportInfo* eri = m_CrashInfo.GetReport(m_nCurReport);  
	if(eri->GetRegKeyCount()!=0)
    {
        m_Assync.SetProgress(_T("Dumping registry keys..."), 0, false);    
    }

	// Walk through our registry key list	
    for(i=0; i<eri->GetRegKeyCount(); i++)
    {
		CString sKeyName;
		ERIRegKey rki;
		eri->GetRegKeyByIndex(i, sKeyName, rki);

        if(m_Assync.IsCancelled())
            goto cleanup;

		CString sFilePath = eri->GetErrorReportDirName() + _T("\\") + rki.m_sDstFileName;

        str.Format(_T("Dumping registry key '%s' to file '%s' "), sKeyName, sFilePath);
        m_Assync.SetProgress(str, 0, false);    

        // Create registry key dump
        CString sErrorMsg;
        DumpRegKey(sKeyName, sFilePath, sErrorMsg);
        ERIFileItem fi;
        fi.m_sSrcFile = sFilePath;
		fi.m_sDestFile = rki.m_sDstFileName;
        fi.m_sDesc = _T("DescRegKey");
        fi.m_bMakeCopy = FALSE;
		fi.m_bAllowDelete = rki.m_bAllowDelete;
        fi.m_sErrorStatus = sErrorMsg;
        std::vector<ERIFileItem> file_list;
        file_list.push_back(fi);
        // Add file to the list of file items
        m_CrashInfo.GetReport(0)->AddFileItem(&fi);
    }
	    
    // Success
    bStatus = TRUE;

cleanup:
		
	// Clean up
    m_Assync.SetProgress(_T("Finished copying files."), 100, false);

    return 0;
}

BOOL CErrorReportSender::CollectSingleFile(ERIFileItem* pfi)
{
	BOOL bStatus = false;
	CString str;
	HANDLE hSrcFile = INVALID_HANDLE_VALUE;
	HANDLE hDestFile = INVALID_HANDLE_VALUE;
	BOOL bGetSize = FALSE;
	LARGE_INTEGER lFileSize;
	LARGE_INTEGER lTotalWritten;
	CString sDestFile;
	BOOL bRead = FALSE;
	BOOL bWrite = FALSE;
	LPBYTE buffer[1024];
	DWORD dwBytesRead = 0;
	DWORD dwBytesWritten = 0;

	CString sErrorReportDir = m_CrashInfo.GetReport(m_nCurReport)->GetErrorReportDirName();

	// Open source file with read/write sharing permissions.
    hSrcFile = CreateFile(pfi->m_sSrcFile, GENERIC_READ, 
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if(hSrcFile==INVALID_HANDLE_VALUE)
    {
        pfi->m_sErrorStatus = Utility::FormatErrorMsg(GetLastError());
        str.Format(_T("Error opening file %s."), pfi->m_sSrcFile);
        m_Assync.SetProgress(str, 0, false);
		goto cleanup;
    }

	// If we should make a copy of the file
    if(pfi->m_bMakeCopy)
    {
        str.Format(_T("Copying file %s."), pfi->m_sSrcFile);
        m_Assync.SetProgress(str, 0, false);		     

        bGetSize = GetFileSizeEx(hSrcFile, &lFileSize);
        if(!bGetSize)
        {
            pfi->m_sErrorStatus = Utility::FormatErrorMsg(GetLastError());
            str.Format(_T("Couldn't get file size of %s"), pfi->m_sSrcFile);
            m_Assync.SetProgress(str, 0, false);
            CloseHandle(hSrcFile);
            hSrcFile = INVALID_HANDLE_VALUE;
            goto cleanup;
        }

        sDestFile = sErrorReportDir + _T("\\") + pfi->m_sDestFile;

        hDestFile = CreateFile(sDestFile, GENERIC_WRITE, 
            FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        if(hDestFile==INVALID_HANDLE_VALUE)
        {
            pfi->m_sErrorStatus = Utility::FormatErrorMsg(GetLastError());
            str.Format(_T("Error creating file %s."), sDestFile);
            m_Assync.SetProgress(str, 0, false);
            CloseHandle(hSrcFile);
            hSrcFile = INVALID_HANDLE_VALUE;
            goto cleanup;
        }

        lTotalWritten.QuadPart = 0;

        for(;;)
        {        
            if(m_Assync.IsCancelled())
                goto cleanup;

            bRead = ReadFile(hSrcFile, buffer, 1024, &dwBytesRead, NULL);
            if(!bRead || dwBytesRead==0)
                break;

            bWrite = WriteFile(hDestFile, buffer, dwBytesRead, &dwBytesWritten, NULL);
            if(!bWrite || dwBytesRead!=dwBytesWritten)
                break;

            lTotalWritten.QuadPart += dwBytesWritten;

            int nProgress = (int)(100.0f*lTotalWritten.QuadPart/lFileSize.QuadPart);

            m_Assync.SetProgress(nProgress, false);
        }

        CloseHandle(hSrcFile);
        hSrcFile = INVALID_HANDLE_VALUE;
        CloseHandle(hDestFile);
        hDestFile = INVALID_HANDLE_VALUE;

		// Use the copy for display and zipping.
		pfi->m_sSrcFile = sDestFile;
    }

	bStatus = true;

cleanup:

    if(hSrcFile!=INVALID_HANDLE_VALUE)
        CloseHandle(hSrcFile);

    if(hDestFile!=INVALID_HANDLE_VALUE)
        CloseHandle(hDestFile);

	
	return bStatus;
}

BOOL CErrorReportSender::CollectFilesBySearchTemplate(ERIFileItem* pfi, std::vector<ERIFileItem>& file_list)
{
	CString sMsg;
	sMsg.Format(_T("Looking for files using search template: %s"), pfi->m_sSrcFile);
	m_Assync.SetProgress(sMsg, 0);

	// Look for files matching search pattern
	WIN32_FIND_DATA ffd;		
	HANDLE hFind = FindFirstFile(pfi->m_sSrcFile, &ffd);
	if (hFind == INVALID_HANDLE_VALUE) 
	{
		// Nothing found
		m_Assync.SetProgress(_T("Could not find any files matching the search template."), 0);
		return FALSE;
	} 
	else
	{		
		// Get owning directory name
		int nPos = pfi->m_sSrcFile.ReverseFind(_T('\\'));
		CString sDir = pfi->m_sSrcFile.Mid(0, nPos);
		
		// Enumerate matching files 
		BOOL bFound = TRUE;
		int nFileCount = 0;
		while(bFound)
		{
			if(m_Assync.IsCancelled())
				break;

			if((ffd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==0)
			{					
				CString sFile = sDir + _T("\\");
				sFile += ffd.cFileName;					

				// Add file to file list.
				ERIFileItem fi;
				fi.m_sSrcFile = sFile;
				fi.m_sDestFile = ffd.cFileName;
				fi.m_sDesc = pfi->m_sDesc;
				fi.m_bMakeCopy = pfi->m_bMakeCopy;
				fi.m_bAllowDelete = pfi->m_bAllowDelete;								
				file_list.push_back(fi);
				
				CollectSingleFile(&fi);

				nFileCount++;								
			}

			// Go to next file
			bFound = FindNextFile(hFind, &ffd);
		}

		// Clean up
		FindClose(hFind);
	}
	
	// Done
	return TRUE;
}

// This method dumps a registry key contents to an XML file
int CErrorReportSender::DumpRegKey(CString sRegKey, CString sDestFile, CString& sErrorMsg)
{
    strconv_t strconv;
    TiXmlDocument document; 

    // Load document if file already exists
    // otherwise create new document.

    FILE* f = NULL;
#if _MSC_VER<1400
    f = _tfopen(sDestFile, _T("rb"));
#else
    _tfopen_s(&f, sDestFile, _T("rb"));
#endif
    if(f!=NULL)
    { 
        document.LoadFile(f);
        fclose(f);
        f = NULL;
    }  

    TiXmlHandle hdoc(&document);

    TiXmlElement* registry = document.RootElement();

    if(registry==NULL)
    {
        registry = new TiXmlElement("registry");
        document.LinkEndChild(registry);
    }

    TiXmlNode* declaration = hdoc.Child(0).ToNode();
    if(declaration==NULL || declaration->Type()!=TiXmlNode::TINYXML_DECLARATION)
    {
        TiXmlDeclaration * decl = new TiXmlDeclaration( "1.0", "UTF-8", "" );
        document.InsertBeforeChild(registry, *decl);
    }   

    DumpRegKey(NULL, sRegKey, registry);

#if _MSC_VER<1400
    f = _tfopen(sDestFile, _T("wb"));
#else
    _tfopen_s(&f, sDestFile, _T("wb"));
#endif
    if(f==NULL)
    {
        sErrorMsg = _T("Error opening file for writing.");
        return 1;
    }

    bool bSave = document.SaveFile(f);

    fclose(f);

    if(!bSave)
    {
        sErrorMsg = _T("Error saving XML document to file: ");
        sErrorMsg += document.ErrorDesc();
    }

    return (bSave==true)?0:1;
}

int CErrorReportSender::DumpRegKey(HKEY hParentKey, CString sSubKey, TiXmlElement* elem)
{
    strconv_t strconv;
    HKEY hKey = NULL;  

    if(hParentKey==NULL)
    {    
        int nSkip = 0;
        if(sSubKey.Left(19).Compare(_T("HKEY_LOCAL_MACHINE\\"))==0)
        {
            hKey = HKEY_LOCAL_MACHINE;
            nSkip = 18;
        }
        else if(sSubKey.Left(18).Compare(_T("HKEY_CURRENT_USER\\"))==0)
        {
            hKey = HKEY_CURRENT_USER;
            nSkip = 17;
        }    
        else 
        {
            return 1; // Invalid key.
        }

        CString sKey = sSubKey.Mid(0, nSkip);
        LPCSTR szKey = strconv.t2utf8(sKey);
        sSubKey = sSubKey.Mid(nSkip+1);

        TiXmlHandle key_node = elem->FirstChild(szKey);
        if(key_node.ToElement()==NULL)
        {
            key_node = new TiXmlElement("k");
            elem->LinkEndChild(key_node.ToNode());
            key_node.ToElement()->SetAttribute("name", szKey);
        }

        DumpRegKey(hKey, sSubKey, key_node.ToElement());
    }
    else
    {
        int pos = sSubKey.Find('\\');
        CString sKey = sSubKey;
        if(pos>0)
            sKey = sSubKey.Mid(0, pos);
        LPCSTR szKey = strconv.t2utf8(sKey);

        TiXmlHandle key_node = elem->FirstChild(szKey);
        if(key_node.ToElement()==NULL)
        {
            key_node = new TiXmlElement("k");
            elem->LinkEndChild(key_node.ToNode());
            key_node.ToElement()->SetAttribute("name", szKey);
        }

        if(ERROR_SUCCESS==RegOpenKeyEx(hParentKey, sKey, 0, GENERIC_READ, &hKey))
        {
            if(pos>0)
            {
                sSubKey = sSubKey.Mid(pos+1);
                DumpRegKey(hKey, sSubKey, key_node.ToElement());
            }
            else
            {
                DWORD dwSubKeys = 0;
                DWORD dwMaxSubKey = 0;
                DWORD dwValues = 0;
                DWORD dwMaxValueNameLen = 0;
                DWORD dwMaxValueLen = 0;
                LONG lResult = RegQueryInfoKey(hKey, NULL, 0, 0, &dwSubKeys, &dwMaxSubKey, 
                    0, &dwValues, &dwMaxValueNameLen, &dwMaxValueLen, NULL, NULL); 
                if(lResult==ERROR_SUCCESS)
                {
                    // Enumerate and dump subkeys          
                    int i;
                    for(i=0; i<(int)dwSubKeys; i++)
                    { 
                        LPWSTR szName = new WCHAR[dwMaxSubKey + 1];
						DWORD dwLen = dwMaxSubKey + 1;
						lResult = RegEnumKeyEx(hKey, i, szName, &dwLen, 0, NULL, 0, NULL);
                        if(lResult==ERROR_SUCCESS)
                        {
                            DumpRegKey(hKey, CString(szName), key_node.ToElement());
                        }

                        delete [] szName;
                    }         

                    // Dump key values 
                    for(i=0; i<(int)dwValues; i++)
                    { 
                        LPWSTR szName = new WCHAR[dwMaxValueNameLen + 1];
						LPBYTE pData = new BYTE[dwMaxValueLen];
						DWORD dwNameLen = dwMaxValueNameLen + 1;
						DWORD dwValueLen = dwMaxValueLen;
						DWORD dwType = 0;

                        lResult = RegEnumValue(hKey, i, szName, &dwNameLen, 0, &dwType, pData, &dwValueLen);
                        if(lResult==ERROR_SUCCESS)
                        {
                            TiXmlHandle val_node = key_node.ToElement()->FirstChild(strconv.w2utf8(szName));
                            if(val_node.ToElement()==NULL)
                            {
                                val_node = new TiXmlElement("v");
                                key_node.ToElement()->LinkEndChild(val_node.ToNode());
                            }

                            val_node.ToElement()->SetAttribute("name", strconv.w2utf8(szName));

                            char str[128] = "";
                            LPSTR szType = NULL;
                            if(dwType==REG_BINARY)
                                szType = "REG_BINARY";
                            else if(dwType==REG_DWORD)
                                szType = "REG_DWORD";
                            else if(dwType==REG_EXPAND_SZ)
                                szType = "REG_EXPAND_SZ";
                            else if(dwType==REG_MULTI_SZ)
                                szType = "REG_MULTI_SZ";
                            else if(dwType==REG_QWORD)
                                szType = "REG_QWORD";
                            else if(dwType==REG_SZ)
                                szType = "REG_SZ";
                            else 
                            {
#if _MSC_VER<1400
                                sprintf(str, "Unknown type (0x%08x)", dwType);
#else
                                sprintf_s(str, 128, "Unknown type (0x%08x)", dwType);
#endif
                                szType = str;                
                            }

                            val_node.ToElement()->SetAttribute("type", szType);              

                            if(dwType==REG_BINARY)
                            {
                                std::string str2;
                                int j;
								for(j=0; j<(int)dwValueLen; j++)
								{
									char num[10];
#if _MSC_VER<1400
									sprintf(num, "%02X", pData[j]);
#else
									sprintf_s(num, 10, "%02X", pData[j]);
#endif
									str2 += num;
									if(j<(int)dwValueLen)
										str2 += " ";
								}

                                val_node.ToElement()->SetAttribute("value", str2.c_str());
                            }
                            else if(dwType==REG_DWORD)
                            {
                                LPDWORD pdwValue = (LPDWORD)pData;
                                char str3[64]="";
#if _MSC_VER<1400
                                sprintf(str3, "0x%08x (%lu)", *pdwValue, *pdwValue);                
#else
                                sprintf_s(str3, 64, "0x%08x (%lu)", *pdwValue, *pdwValue);                
#endif
                                val_node.ToElement()->SetAttribute("value", str3);
                            }
                            else if(dwType==REG_SZ || dwType==REG_EXPAND_SZ)
                            {
                                LPCSTR szValue = strconv.t2utf8((LPCTSTR)pData);
                                val_node.ToElement()->SetAttribute("value", szValue);                
                            }
                            else if(dwType==REG_MULTI_SZ)
                            {                
                                LPCTSTR szValues = (LPCTSTR)pData;
                                int prev = 0;
                                int pos2 = 0;
                                for(;;)
                                {
                                    if(szValues[pos2]==0)
                                    {
                                        CString sValue = CString(szValues+prev, pos2-prev);
                                        LPCSTR szValue = strconv.t2utf8(sValue);

                                        TiXmlHandle str_node = new TiXmlElement("str");
                                        val_node.ToElement()->LinkEndChild(str_node.ToNode());                    
                                        str_node.ToElement()->SetAttribute("value", szValue);              

                                        prev = pos2+1;
                                    }

                                    if(szValues[pos2]==0 && szValues[pos2+1]==0)
                                        break; // Double-null

                                    pos2++;
                                }                     
                            }
                        }

                        delete [] szName;
                        delete [] pData;
                    }         
                }
            }

            RegCloseKey(hKey);
        }
        else
        {      
            CString sErrMsg = Utility::FormatErrorMsg(GetLastError());
            LPCSTR szErrMsg = strconv.t2utf8(sErrMsg);
            key_node.ToElement()->SetAttribute("error", szErrMsg);
        }
    }

    return 0;
}

// This method calculates an MD5 hash for the file
int CErrorReportSender::CalcFileMD5Hash(CString sFileName, CString& sMD5Hash)
{
    FILE* f = NULL;  // Handle to file
    BYTE buff[512];  // Read buffer
    MD5 md5;         // MD5 hash
    MD5_CTX md5_ctx; // MD5 context
    unsigned char md5_hash[16]; // MD5 hash as sequence of bytes
    int i;

	// Clear output
    sMD5Hash.Empty();

	// Open file
#if _MSC_VER<1400
    f = _tfopen(sFileName.GetBuffer(0), _T("rb"));
#else
    _tfopen_s(&f, sFileName.GetBuffer(0), _T("rb"));
#endif

	// Check if file has been opened
    if(f==NULL) 
        return -1;

	// Init MD5 context
    md5.MD5Init(&md5_ctx);

	// Read file contents and update MD5 hash as each portion is being read
    while(!feof(f))
    {
        size_t count = fread(buff, 1, 512, f);
        if(count>0)
        {
            md5.MD5Update(&md5_ctx, buff, (unsigned int)count);
        }
    }

	// Close file
    fclose(f);

	// Finalize MD5 hash calculation
    md5.MD5Final(md5_hash, &md5_ctx);

	// Format hash as a string
    for(i=0; i<16; i++)
    {
        CString number;
        number.Format(_T("%02x"), md5_hash[i]);
        sMD5Hash += number;
    }

	// Done
    return 0;
}

// This method restarts the client application
BOOL CErrorReportSender::RestartApp()
{
	// Check our config - if we should restart the client app or not?
    if(m_CrashInfo.m_bAppRestart==FALSE)
        return FALSE; // No need to restart

	// Reset restart flag to avoid restarting the app twice
	// (if app has been restarted already).
	m_CrashInfo.m_bAppRestart = FALSE;

	// Add a message to log and reset progress 
    m_Assync.SetProgress(_T("Restarting the application..."), 0, false);

	// Set up process start up info
    STARTUPINFO si;
    memset(&si, 0, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);

	// Set up process information
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(PROCESS_INFORMATION));  

	// Format command line
    CString sCmdLine;
    if(m_CrashInfo.m_sRestartCmdLine.IsEmpty())
    {
        // Format with double quotes to avoid first empty parameter
        sCmdLine.Format(_T("\"%s\""), m_CrashInfo.GetReport(m_nCurReport)->GetImageName());
    }
    else
    {
		// Format with double quotes to avoid first empty parameters
        sCmdLine.Format(_T("\"%s\" %s"), m_CrashInfo.GetReport(m_nCurReport)->GetImageName(), 
            m_CrashInfo.m_sRestartCmdLine.GetBuffer(0));
    }

	// Create process using the command line prepared earlier
    BOOL bCreateProcess = CreateProcess(
        m_CrashInfo.GetReport(m_nCurReport)->GetImageName(), 
		sCmdLine.GetBuffer(0), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    
	// The following is to avoid a handle leak
	if(pi.hProcess)
    {
        CloseHandle(pi.hProcess);
        pi.hProcess = NULL;
    }

	// The following is to avoid a handle leak
    if(pi.hThread)
    {
        CloseHandle(pi.hThread);
        pi.hThread = NULL;
    }

	// Check if process was created
    if(!bCreateProcess)
    {    
		// Add error message
        m_Assync.SetProgress(_T("Error restarting the application!"), 0, false);
        return FALSE;
    }

	// Success
    m_Assync.SetProgress(_T("Application restarted OK."), 0, false);
    return TRUE;
}

// This method compresses the files contained in the report and produces a ZIP archive.
BOOL CErrorReportSender::CompressReportFiles(CErrorReportInfo* eri)
{ 
    BOOL bStatus = FALSE;
    strconv_t strconv;
    zipFile hZip = NULL;
    CString sMsg;
    LONG64 lTotalSize = 0;
    LONG64 lTotalCompressed = 0;
    BYTE buff[1024];
    DWORD dwBytesRead=0;
    HANDLE hFile = INVALID_HANDLE_VALUE;  
    std::map<CString, ERIFileItem>::iterator it;
    FILE* f = NULL;
    CString sMD5Hash;

	// Add a different log message depending on the current mode.
    if(m_bExport)
        m_Assync.SetProgress(_T("[exporting_report]"), 0, false);
    else
        m_Assync.SetProgress(_T("[compressing_files]"), 0, false);

	// Calculate the total size of error report files
	lTotalSize = eri->GetTotalSize();

	// Add a message to log
    sMsg.Format(_T("Total file size for compression is %I64d bytes"), lTotalSize);
    m_Assync.SetProgress(sMsg, 0, false);

	// Determine what name to use for the output ZIP archive file.
    if(m_bExport)
        m_sZipName = m_sExportFileName;  
    else
        m_sZipName = eri->GetErrorReportDirName() + _T(".zip");  

	// Update progress
    sMsg.Format(_T("Creating ZIP archive file %s"), m_sZipName);
    m_Assync.SetProgress(sMsg, 1, false);

	// Create ZIP archive
    hZip = zipOpen((const char*)m_sZipName.GetBuffer(0), APPEND_STATUS_CREATE);
    if(hZip==NULL)
    {
        m_Assync.SetProgress(_T("Failed to create ZIP file."), 100, true);
        goto cleanup;
    }

	// Enumerate files contained in the report
	int i;
	for(i=0; i<eri->GetFileItemCount(); i++)
    { 
		ERIFileItem* pfi = eri->GetFileItemByIndex(i);

		// Check if the operation was cancelled by user
        if(m_Assync.IsCancelled())    
            goto cleanup;

		// Define destination file name in ZIP archive
        CString sDstFileName = pfi->m_sDestFile.GetBuffer(0);
		// Define source file name
        CString sFileName = pfi->m_sSrcFile.GetBuffer(0);
		// Define file description
        CString sDesc = pfi->m_sDesc;

		// Update progress
        sMsg.Format(_T("Compressing file %s"), sDstFileName);
        m_Assync.SetProgress(sMsg, 0, false);

		// Open file for reading
        hFile = CreateFile(sFileName, 
            GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, NULL, NULL); 
        if(hFile==INVALID_HANDLE_VALUE)
        {
            sMsg.Format(_T("Couldn't open file %s"), sFileName);
            m_Assync.SetProgress(sMsg, 0, false);
            continue;
        }

		// Get file information.
        BY_HANDLE_FILE_INFORMATION fi;
        GetFileInformationByHandle(hFile, &fi);

		// Convert file creation time to system file time.
        SYSTEMTIME st;
        FileTimeToSystemTime(&fi.ftLastWriteTime, &st);

		// Fill in the ZIP file info
        zip_fileinfo info;
        info.dosDate = 0;
        info.tmz_date.tm_year = st.wYear;
        info.tmz_date.tm_mon = st.wMonth-1;
        info.tmz_date.tm_mday = st.wDay;
        info.tmz_date.tm_hour = st.wHour;
        info.tmz_date.tm_min = st.wMinute;
        info.tmz_date.tm_sec = st.wSecond;
        info.external_fa = FILE_ATTRIBUTE_NORMAL;
        info.internal_fa = FILE_ATTRIBUTE_NORMAL;

		// Create new file inside of our ZIP archive
        int n = zipOpenNewFileInZip( hZip, (const char*)strconv.t2a(sDstFileName.GetBuffer(0)), &info,
            NULL, 0, NULL, 0, strconv.t2a(sDesc), Z_DEFLATED, Z_DEFAULT_COMPRESSION);
        if(n!=0)
        {
            sMsg.Format(_T("Couldn't compress file %s"), sDstFileName);
            m_Assync.SetProgress(sMsg, 0, false);
            continue;
        }

		// Read source file contents and write it to ZIP archive
        for(;;)
        {
			// Check if operation was cancelled by user
            if(m_Assync.IsCancelled())    
                goto cleanup;

			// Read a portion of source file
            BOOL bRead = ReadFile(hFile, buff, 1024, &dwBytesRead, NULL);
            if(!bRead || dwBytesRead==0)
                break;

			// Write a portion into destination file
            int res = zipWriteInFileInZip(hZip, buff, dwBytesRead);
            if(res!=0)
            {
                zipCloseFileInZip(hZip);
                sMsg.Format(_T("Couldn't write to compressed file %s"), sDstFileName);
                m_Assync.SetProgress(sMsg, 0, false);        
                break;
            }

			// Update totals
            lTotalCompressed += dwBytesRead;

			// Update progress
            float fProgress = 100.0f*lTotalCompressed/lTotalSize;
            m_Assync.SetProgress((int)fProgress, false);
        }

		// Close file
        zipCloseFileInZip(hZip);
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
    }

	// Close ZIP archive
    if(hZip!=NULL)
    {
        zipClose(hZip, NULL);
        hZip = NULL;
    }

    // Save MD5 hash file
    if(!m_bExport)
    {
        sMsg.Format(_T("Calculating MD5 hash for file %s"), m_sZipName);
        m_Assync.SetProgress(sMsg, 0, false);

        int nCalcMD5 = CalcFileMD5Hash(m_sZipName, sMD5Hash);
        if(nCalcMD5!=0)
        {
            sMsg.Format(_T("Couldn't calculate MD5 hash for file %s"), m_sZipName);
            m_Assync.SetProgress(sMsg, 0, false);
            goto cleanup;
        }

#if _MSC_VER <1400
        f = _tfopen(m_sZipName + _T(".md5"), _T("wt"));
#else
        _tfopen_s(&f, m_sZipName + _T(".md5"), _T("wt"));
#endif
        if(f==NULL)
        {
            sMsg.Format(_T("Couldn't save MD5 hash for file %s"), m_sZipName);
            m_Assync.SetProgress(sMsg, 0, false);
            goto cleanup;
        }

        _ftprintf(f, sMD5Hash);
        fclose(f);
        f = NULL;
    }

	// Check if totals match
    if(lTotalSize==lTotalCompressed)
        bStatus = TRUE;

cleanup:

	// Clean up

    if(hZip!=NULL)
        zipClose(hZip, NULL);

    if(hFile!=INVALID_HANDLE_VALUE)
        CloseHandle(hFile);

    if(f!=NULL)
        fclose(f);

    if(bStatus)
        m_Assync.SetProgress(_T("Finished compressing files...OK"), 100, true);
    else
        m_Assync.SetProgress(_T("File compression failed."), 100, true);

    if(m_bExport)
    {
        if(bStatus)
            m_Assync.SetProgress(_T("[end_exporting_report_ok]"), 100, false);    
        else
            m_Assync.SetProgress(_T("[end_exporting_report_failed]"), 100, false);    
    }
    else
    {    
        m_Assync.SetProgress(_T("[end_compressing_files]"), 100, false);   
    }

    return bStatus;
}

// This method sends the error report over the Internet
BOOL CErrorReportSender::SendReport()
{
    int status = 1;

	InitLog();

    m_Assync.SetProgress(_T("[sending_report]"), 0);

	// Arrange priorities in reverse order
    std::multimap<int, int> order;

	std::pair<int, int> pair1(m_CrashInfo.m_uPriorities[CR_HTTP], CR_HTTP);
    order.insert(pair1);

    std::multimap<int, int>::reverse_iterator rit;

	// Walk through priorities
    for(rit=order.rbegin(); rit!=order.rend(); rit++)
    {
        m_Assync.SetProgress(_T("[sending_attempt]"), 0);
        m_SendAttempt++;    

		// Check if operation was cancelled.
        if(m_Assync.IsCancelled()){ break; }

        int id = rit->second;

        BOOL bResult = FALSE;

		// Send the report
        if(id==CR_HTTP)
            bResult = SendOverHTTP();  
    
		// Check if this attempt has failed
        if(bResult==FALSE)
            continue;
		// else wait for completion
        if(0==m_Assync.WaitForCompletion())
        {
            status = 0;
            break;
        }
    }

    // Remove compressed ZIP file and MD5 file
    Utility::RecycleFile(m_sZipName, true);
    Utility::RecycleFile(m_sZipName+_T(".md5"), true);

	// Check status
    if(status==0)
    {
		// Success
        m_Assync.SetProgress(_T("[status_success]"), 0);
        m_CrashInfo.GetReport(m_nCurReport)->SetDeliveryStatus(DELIVERED);
        // Delete report files
        Utility::RecycleFile(m_CrashInfo.GetReport(m_nCurReport)->GetErrorReportDirName(), true);    
    }
    else
    {
		// Some error occurred
        m_CrashInfo.GetReport(m_nCurReport)->SetDeliveryStatus(FAILED);    
        m_Assync.SetProgress(_T("[status_failed]"), 0);    

        // Check if we should store files for later delivery or we should remove them
        if(!m_CrashInfo.m_bQueueEnabled)
        {
            // Delete report files
            Utility::RecycleFile(m_CrashInfo.GetReport(m_nCurReport)->GetErrorReportDirName(), true);    
        }
    }

	// Done
    m_nStatus = status;
    m_Assync.SetCompleted(status);  
    return status==0?TRUE:FALSE;
}

// This method sends the report over HTTP request
BOOL CErrorReportSender::SendOverHTTP()
{  
    strconv_t strconv;

	// Check our config - should we send the report over HTTP or not?
    if(m_CrashInfo.m_uPriorities[CR_HTTP]==CR_NEGATIVE_PRIORITY)
    {
        m_Assync.SetProgress(_T("Sending error report over HTTP is disabled (negative priority); skipping."), 0);
        return FALSE;
    }

	// Check URL
    if(m_CrashInfo.m_sUrl.IsEmpty())
    {
        m_Assync.SetProgress(_T("No URL specified for sending error report over HTTP; skipping."), 0);
        return FALSE;
    }

	// Update progress
    m_Assync.SetProgress(_T("Sending error report over HTTP..."), 0);
    m_Assync.SetProgress(_T("Preparing HTTP request data..."), 0);

	// Create HTTP request
    CHttpRequest request;
    request.m_sUrl = m_CrashInfo.m_sUrl;  

	CErrorReportInfo* eri = m_CrashInfo.GetReport(m_nCurReport);

	// Fill in the request fields
	CString sNum;
	sNum.Format(_T("%d"), CRASHRPT_VER);
	request.m_aTextFields[_T("crashrptver")] = strconv.t2utf8(sNum);
    request.m_aTextFields[_T("appname")] = strconv.t2utf8(eri->GetAppName());
    request.m_aTextFields[_T("appversion")] = strconv.t2utf8(eri->GetAppVersion());
    request.m_aTextFields[_T("crashguid")] = strconv.t2utf8(eri->GetCrashGUID());
    request.m_aTextFields[_T("emailfrom")] = strconv.t2utf8(eri->GetEmailFrom());
    request.m_aTextFields[_T("emailsubject")] = strconv.t2utf8(m_CrashInfo.m_sEmailSubject);
    request.m_aTextFields[_T("description")] = strconv.t2utf8(eri->GetProblemDescription());
	request.m_aTextFields[_T("exceptionmodule")] = strconv.t2utf8(eri->GetExceptionModule());
	request.m_aTextFields[_T("exceptionmoduleversion")] = strconv.t2utf8(eri->GetExceptionModuleVersion());	
	sNum.Format(_T("%I64u"), eri->GetExceptionModuleBase());
	request.m_aTextFields[_T("exceptionmodulebase")] = strconv.t2utf8(sNum);
	sNum.Format(_T("%I64u"), eri->GetExceptionAddress());
	request.m_aTextFields[_T("exceptionaddress")] = strconv.t2utf8(sNum);

	// Add an MD5 hash of file attachment
    CString sMD5Hash;
    CalcFileMD5Hash(m_sZipName, sMD5Hash);
    request.m_aTextFields[_T("md5")] = strconv.t2utf8(sMD5Hash);

	// Set content type 
    CHttpRequestFile f;
    f.m_sSrcFileName = m_sZipName;
    f.m_sContentType = _T("application/zip");  
    request.m_aIncludedFiles[_T("crashrpt")] = f;  

	// Send HTTP request assynchronously
    BOOL bSend = m_HttpSender.SendAssync(request, &m_Assync);  
    return bSend;
}

int CErrorReportSender::Base64EncodeAttachment(CString sFileName, 
                                               std::string& sEncodedFileData)
{
    strconv_t strconv;

    int uFileSize = 0;
    BYTE* uchFileData = NULL;  
    struct _stat st;  

	// Get file information
    int nResult = _tstat(sFileName, &st);
    if(nResult != 0)
        return 1;  // File not found.

    // Allocate buffer of file size
    uFileSize = (int)st.st_size;
    uchFileData = new BYTE[uFileSize];

    // Read file data to buffer.
    FILE* f = NULL;
#if _MSC_VER<1400
    f = _tfopen(sFileName, _T("rb"));
#else
    /*errno_t err = */_tfopen_s(&f, sFileName, _T("rb"));  
#endif 

    if(!f || fread(uchFileData, uFileSize, 1, f)!=1)
    {
        delete [] uchFileData;
        uchFileData = NULL;
        return 2; // Coudln't read file data.
    }

	// Close file
    fclose(f);

	// BASE-64 ecode data
    sEncodedFileData = base64_encode(uchFileData, uFileSize);

	// Clean up
    delete [] uchFileData;

    // OK.
    return 0;
}

CString CErrorReportSender::GetLangStr(LPCTSTR szSection, LPCTSTR szName)
{
	return Utility::GetINIString(m_CrashInfo.m_sLangFileName, szSection, szName);
}

void CErrorReportSender::ExportReport(LPCTSTR szOutFileName)
{
	SetExportFlag(TRUE, szOutFileName);
    DoWork(COMPRESS_REPORT);    
}

BOOL CErrorReportSender::SendRecentReports()
{
	// This method sends all queued error reports in turn.
	m_bSendingNow = TRUE;
	m_bErrors = FALSE;
		
	// Send error reports in turn
	BOOL bSend = TRUE;
	int nReport = -1;
	while(bSend)
	{
		// Ask GUI for hint what report to send next.
		// This is needed to send error report in the same order they appear in the list
		// (list may be sorted in different order).
		if(IsWindow(m_hWndNotify))
			nReport = (int)::SendMessage(m_hWndNotify, WM_NEXT_ITEM_HINT, 0, 0);

		bSend = SendNextReport(nReport);
	}
	
	// Close log
	m_Assync.CloseLogFile();

	// Notify GUI about completion
	if(IsWindow(m_hWndNotify))
		::PostMessage(m_hWndNotify, WM_DELIVERY_COMPLETE, 0, 0);	

	// Done
	m_bSendingNow = FALSE;
    return TRUE;
}

BOOL CErrorReportSender::SendNextReport(int nReport)
{  	
	if(m_Assync.IsCancelled())
		return FALSE;	// Return FALSE to prevent sending next report

	CErrorReportInfo* eri = NULL;
	if(nReport != -1)
	{
		eri = m_CrashInfo.GetReport(nReport);
		if(!eri->IsSelected() || eri->GetDeliveryStatus()!=PENDING)
			eri = NULL;
	}

	if(eri==NULL)
	{
		// Walk through error reports 
		int i;
		for(i=0; i<m_CrashInfo.GetReportCount(); i++)
		{
			if(!m_CrashInfo.GetReport(i)->IsSelected())
				continue; // Skip this (not selected item)
			            
			if(m_CrashInfo.GetReport(i)->GetDeliveryStatus()==PENDING)
			{
				nReport = i;
				eri = m_CrashInfo.GetReport(i);
				break;
			}
		}
	}
    
	if(eri==NULL)
	{
		// Return FALSE to prevent sending next report
		return FALSE;
	}

	// Save current report index			
	m_nCurReport = nReport;

	eri->SetDeliveryStatus(INPROGRESS);

	// Notify GUI about current item change
	if(IsWindow(m_hWndNotify))
		::PostMessage(m_hWndNotify, WM_ITEM_STATUS_CHANGED, (WPARAM)m_nCurReport, (LPARAM)eri->GetDeliveryStatus());	
						
    // Add a message to log
	CString sMsg;
	sMsg.Format(_T(">>> Performing actions with error report: '%s'"), 
					m_CrashInfo.GetReport(m_nCurReport)->GetErrorReportDirName());
	m_Assync.SetProgress(sMsg, 0, false);

	// Send report
    if(!DoWork(COMPRESS_REPORT|SEND_REPORT))
	{
		m_bErrors = TRUE;
		eri->SetDeliveryStatus(FAILED);
	}
	else
	{
		eri->SetDeliveryStatus(DELIVERED);
	}

	// Notify GUI about current item change
	if(IsWindow(m_hWndNotify))
		::PostMessage(m_hWndNotify, WM_ITEM_STATUS_CHANGED, (WPARAM)m_nCurReport, (LPARAM)eri->GetDeliveryStatus());

	// Return TRUE to indicate next report can be sent
    return TRUE;
}

BOOL CErrorReportSender::IsSendingNow()
{
	// Return TRUE if currently sending error report(s)
	return m_bSendingNow;
}


BOOL CErrorReportSender::HasErrors()
{
	return m_bErrors;
}

CString CErrorReportSender::GetLogFilePath()
{
	// Return path to log file
	return m_Assync.GetLogFilePath();
}

int CErrorReportSender::TerminateAllCrashSenderProcesses()
{
	// This method looks for all runing CrashSender.exe processes
	// and terminates each one. This may be needed when an application's installer
	// wants to shutdown all crash sender processes running in background
	// to replace the locked files.
	
	// Format process name.
	CString sProcessName;
#ifdef _DEBUG
	sProcessName.Format(_T("CrashSender%dd.exe"), CRASHRPT_VER);
#else
	sProcessName.Format(_T("CrashSender%d.exe"), CRASHRPT_VER);
#endif

	PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);

	// Create the list of all processes in the system
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);

    if (Process32First(snapshot, &entry) == TRUE)
    {
		// Walk through processes
        while (Process32Next(snapshot, &entry) == TRUE)
        {
			// Compare process name
            if (_tcsicmp(entry.szExeFile, sProcessName) == 0 &&
				entry.th32ProcessID != GetCurrentProcessId())
            {  				
				// Open process handle
                HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);

				// Terminate process.
                TerminateProcess(hProcess, 1);

                CloseHandle(hProcess);
            }
        }
    }

	// Clean up
    CloseHandle(snapshot);

    return 0;
}

