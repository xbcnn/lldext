
#include "DbgExts.h"

HRESULT CALLBACK injectdll(IDebugClient* pDebugClient, PCSTR args)
{
    IDebugControl* pDebugControl;
    if (SUCCEEDED(pDebugClient->QueryInterface(__uuidof(IDebugControl), (void **)&pDebugControl))) {
        if (strlen(args) == 0) {
            pDebugControl->Output(DEBUG_OUTPUT_NORMAL, "DLL path is required.");
            pDebugControl->Release();
            return S_OK;
        }
        InjectionControl *pInjectionEngine = new InjectionControlImpl(pDebugClient);
        pInjectionEngine->Inject(args);

        pDebugControl->Release();
    }
    return S_OK;
}


InjectionControl::InjectionControl(IDebugClient * pOriginalDebugClient)
{
    CheckHResult(pOriginalDebugClient->CreateClient(&m_pDebugClient));
    CheckHResult(m_pDebugClient->QueryInterface(__uuidof(IDebugControl), (void **)&m_pDebugControl));
    CheckHResult(m_pDebugClient->QueryInterface(__uuidof(IDebugSystemObjects), (void **)&m_pDebugSystemObjects));
    CheckHResult(m_pDebugClient->QueryInterface(__uuidof(IDebugSymbols), (void **)&m_pDebugSymbols));
    CheckHResult(m_pDebugClient->QueryInterface(__uuidof(IDebugAdvanced), (void **)&m_pDebugAdvanced));
    CheckHResult(m_pDebugClient->QueryInterface(__uuidof(IDebugRegisters), (void **)&m_pDebugRegisters));
}

InjectionControl::~InjectionControl(void)
{
    m_pDebugClient->Release();
    m_pDebugControl->Release();
    m_pDebugSystemObjects->Release();
    m_pDebugSymbols->Release();
    m_pDebugAdvanced->Release();
    m_pDebugRegisters->Release();
}

// IUnknown.
STDMETHODIMP InjectionControl::QueryInterface(REFIID interfaceId, PVOID* instance)
{
    if (interfaceId == __uuidof(IUnknown) || interfaceId == __uuidof(IDebugEventCallbacks)) {
        *instance = this;
        // No need to refcount as this class is contained.
        return S_OK;
    } else {
        *instance = NULL;
        return E_NOINTERFACE;
    }
}

void InjectionControl::SuspendAllThreadsButCurrent(void)
{
    m_pDebugControl->Execute(DEBUG_OUTPUT_NORMAL, "~*n", DEBUG_EXECUTE_NOT_LOGGED);
    m_pDebugControl->Execute(DEBUG_OUTPUT_NORMAL, "~m", DEBUG_EXECUTE_NOT_LOGGED);
}

void InjectionControl::ResumeAllThreads(void)
{
    m_pDebugControl->Execute(DEBUG_OUTPUT_NORMAL, "~n", DEBUG_EXECUTE_NOT_LOGGED);
    m_pDebugControl->Execute(DEBUG_OUTPUT_NORMAL, "~*m", DEBUG_EXECUTE_NOT_LOGGED);
}

void InjectionControl::Inject(PCSTR dllName)
{
    SuspendAllThreadsButCurrent();

    m_pDebugAdvanced->GetThreadContext(&m_threadContext, sizeof(CONTEXT));

    ULONG64 hProcess;

    // find the LoadLibrary function (on Win7 we need to use kernel32, on Win8+ kernelbase)
    ULONG64 offset;
    if (FAILED(m_pDebugSymbols->GetOffsetByName("kernelbase!LoadLibraryA", &offset))) {
        CheckHResult(m_pDebugSymbols->GetOffsetByName("kernel32!LoadLibraryA", &offset));
    }

    CheckHResult(m_pDebugSystemObjects->GetCurrentProcessHandle(&hProcess));

    size_t dllNameLength = strlen(dllName) + 1;
    SIZE_T n;
    // allocate injection buffer
    PBYTE injectionBuffer = (PBYTE)VirtualAllocEx((HANDLE)hProcess, nullptr, dllNameLength + GetPayloadSize(),
        MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!injectionBuffer) {
        throw ::Exception(HRESULT_FROM_WIN32(GetLastError()));
    }
    if (!WriteProcessMemory((HANDLE)hProcess, injectionBuffer, dllName, dllNameLength, &n)) {
        throw ::Exception(HRESULT_FROM_WIN32(GetLastError()));
    }
    if (!WriteProcessMemory((HANDLE)hProcess, injectionBuffer + dllNameLength, GetPayload(), GetPayloadSize(), &n)) {
        throw ::Exception(HRESULT_FROM_WIN32(GetLastError()));
    }

    // platform specific settings
    CONTEXT tempContext;
    PrepareInjectionContext(&tempContext, injectionBuffer, dllNameLength, offset);

    CheckHResult(m_pDebugAdvanced->SetThreadContext(&tempContext, sizeof(CONTEXT)));

    CheckHResult(m_pDebugClient->SetEventCallbacks(this));

    // run the only thread - should break after the injection
    CheckHResult(m_pDebugControl->Execute(DEBUG_OUTPUT_NORMAL, "g", DEBUG_EXECUTE_NOT_LOGGED));
}

STDMETHODIMP_(ULONG) InjectionControl::AddRef() { return S_OK; }

STDMETHODIMP_(ULONG) InjectionControl::Release() { return S_OK; }


STDMETHODIMP InjectionControl::GetInterestMask(PULONG Mask)
{
    *Mask = DEBUG_EVENT_EXCEPTION;
    return S_OK;
}

STDMETHODIMP InjectionControl::Breakpoint(PDEBUG_BREAKPOINT Bp)
{
    return DEBUG_STATUS_GO;
}

STDMETHODIMP InjectionControl::Exception(PEXCEPTION_RECORD64 Exception, ULONG FirstChance)
{
    if (IsBreakpointOffsetHit()) {
        m_pDebugAdvanced->SetThreadContext(&m_threadContext, sizeof(CONTEXT));
        ResumeAllThreads();

        m_pDebugClient->SetEventCallbacks(nullptr);
        delete this;
    }
    return DEBUG_STATUS_GO;
}

STDMETHODIMP InjectionControl::CreateThread(ULONG64 Handle, ULONG64 DataOffset,
    ULONG64 StartOffset)
{
    return DEBUG_STATUS_GO;
}
STDMETHODIMP InjectionControl::ExitThread(ULONG ExitCode)
{
    return DEBUG_STATUS_GO;
}

STDMETHODIMP InjectionControl::CreateProcess(ULONG64 ImageFileHandle, ULONG64 Handle, ULONG64 BaseOffset,
    ULONG ModuleSize, PCSTR ModuleName, PCSTR ImageName, ULONG CheckSum, ULONG TimeDateStamp,
    ULONG64 InitialThreadHandle, ULONG64 ThreadDataOffset, ULONG64 StartOffset)
{
    return DEBUG_STATUS_GO;
}

STDMETHODIMP InjectionControl::ExitProcess(ULONG ExitCode)
{
    return DEBUG_STATUS_GO;
}

// Any of these values may be zero.
STDMETHODIMP InjectionControl::LoadModule(ULONG64 ImageFileHandle, ULONG64 BaseOffset, ULONG ModuleSize,
    PCSTR ModuleName, PCSTR ImageName, ULONG CheckSum, ULONG TimeDateStamp)
{
    return DEBUG_STATUS_GO;
}

STDMETHODIMP InjectionControl::UnloadModule(__in_opt PCSTR ImageBaseName, ULONG64 BaseOffset)
{
    return DEBUG_STATUS_GO;
}

STDMETHODIMP InjectionControl::SystemError(ULONG Error, ULONG Level)
{
    return DEBUG_STATUS_GO;
}

STDMETHODIMP InjectionControl::SessionStatus(ULONG Status)
{
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP InjectionControl::ChangeDebuggeeState(ULONG Flags, ULONG64 Argument)
{
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP InjectionControl::ChangeEngineState(ULONG Flags, ULONG64 Argument)
{
    return DEBUG_STATUS_NO_CHANGE;
}

STDMETHODIMP InjectionControl::ChangeSymbolState(ULONG Flags, ULONG64 Argument)
{
    return DEBUG_STATUS_NO_CHANGE;
}

PBYTE InjectionControlImpl::GetPayload(void)
{
    return m_payload;
}

size_t InjectionControlImpl::GetPayloadSize(void)
{
    return sizeof(m_payload);
}

bool InjectionControlImpl::IsBreakpointOffsetHit(void)
{
    ULONG64 offset;
    CheckHResult(m_pDebugRegisters->GetInstructionOffset(&offset));

    return offset == m_breakOffset;
}

#if _WIN64

/* ********************** x64 **************************** */

// call rax
// int 3
BYTE InjectionControlImpl::m_payload[PAYLOAD_SIZE] = { 0xff, 0xD0, 0xCC };


void InjectionControlImpl::PrepareInjectionContext(CONTEXT *pTempContext, PBYTE injectionBuffer,
    size_t dllNameLength, ULONG64 offset)
{
    CopyMemory(pTempContext, &m_threadContext, sizeof(CONTEXT));
    // rax must be set to LoadLibrary address
    // rcx must be set to the address of the dllname
    // rip must be set to the address of the code to execute
    pTempContext->Rax = offset;
    pTempContext->Rcx = (DWORD64)injectionBuffer;
    pTempContext->Rip = (DWORD64)(injectionBuffer + dllNameLength);
    m_breakOffset = (DWORD64)(injectionBuffer + dllNameLength + sizeof(m_payload) - 1);
}

#else

/* ********************** x86 **************************** */

// call rax
// int 3
BYTE InjectionControlImpl::m_payload[PAYLOAD_SIZE] = { 0x51, 0xff, 0xD0, 0xCC };

void InjectionControlImpl::PrepareInjectionContext(CONTEXT *pTempContext, PBYTE injectionBuffer,
    size_t dllNameLength, ULONG64 offset)
{
    CopyMemory(pTempContext, &m_threadContext, sizeof(CONTEXT));
    // rax must be set to LoadLibrary address
    // we need to push the dll name on the stack
    // rip must be set to the address of the code to execute
    pTempContext->Eax = (DWORD)offset;
    pTempContext->Ecx = (DWORD)injectionBuffer;
    pTempContext->Eip = (DWORD)(injectionBuffer + dllNameLength);
    m_breakOffset = (DWORD)(injectionBuffer + dllNameLength + sizeof(m_payload) - 1);
}

#endif
