#include <windows.h>
#include <ocidl.h>
#include <xamlom.h>
#include <wrl.h>
#include <thread>
#include <atomic>
#include <mutex>
#include "xaml_bridge.h"
#include "dock_animation.h"
#include "../common/diagnostics.h"

using namespace Microsoft::WRL;
namespace {
const CLSID bridgeClass = {0x5031c518, 0x20a6, 0x4b63, {0x98, 0x1c, 0x54, 0x2e, 0x57, 0x28, 0xab, 0xe1}};
std::atomic<bool> enabled{false};

class Site;
static ComPtr<Site> s_activeSite;
static std::mutex s_siteMutex;

class Site final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IObjectWithSite,
    IVisualTreeServiceCallback2> {
public:
    ComPtr<IXamlDiagnostics> diagnostics;
    ComPtr<IVisualTreeService3> tree;

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if (!site) return S_OK;
        HRESULT hr = site->QueryInterface(IID_PPV_ARGS(&diagnostics));
        if (FAILED(hr)) return hr;
        hr = site->QueryInterface(IID_PPV_ARGS(&tree));
        if (FAILED(hr)) return hr;

        {
            std::lock_guard<std::mutex> lock(s_siteMutex);
            s_activeSite = this;
        }

        ComPtr<Site> self(this);
        std::thread([self]() {
            HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            HRESULT result = self->tree->AdviseVisualTreeChange(self.Get());
            WriteDiagnostic("explorer", "XAML AdviseVisualTreeChange hr=" + std::to_string(result));
            if (SUCCEEDED(apartment)) CoUninitialize();
        }).detach();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        return diagnostics ? diagnostics->QueryInterface(iid, result) : E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation, VisualElement element,
        VisualMutationType mutation) override {
        const bool frame = element.Type && (wcscmp(element.Type, L"Taskbar.TaskbarFrame") == 0 ||
            wcscmp(element.Type, L"SystemTray.SystemTrayFrame") == 0);
        if (enabled && mutation == Add && frame) {
            ComPtr<IInspectable> object;
            HRESULT hr = diagnostics->GetIInspectableFromHandle(element.Handle, &object);
            WriteDiagnostic("explorer", "XAML frame discovered hr=" + std::to_string(hr));
            if (SUCCEEDED(hr)) Lightency::DockAnimation::AttachXamlElement(object.Get());
        }
        if (mutation == Add) {
            SysFreeString(element.Name);
            SysFreeString(element.Type);
            SysFreeString(element.SrcInfo.FileName);
            SysFreeString(element.SrcInfo.Hash);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) override {
        return S_OK;
    }
};

class Factory final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto site = Make<Site>();
        return site ? site->QueryInterface(iid, result) : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (clsid != bridgeClass) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = Make<Factory>();
    return factory ? factory->QueryInterface(iid, result) : E_OUTOFMEMORY;
}

STDAPI DllCanUnloadNow() { return S_FALSE; }

bool Lightency::XamlBridge::Initialize() {
    enabled = true;

    {
        std::lock_guard<std::mutex> lock(s_siteMutex);
        if (s_activeSite && s_activeSite->tree) {
            ComPtr<Site> site = s_activeSite;
            std::thread([site]() {
                HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                HRESULT result = site->tree->AdviseVisualTreeChange(site.Get());
                WriteDiagnostic("explorer", "XAML re-AdviseVisualTreeChange hr=" + std::to_string(result));
                if (SUCCEEDED(apartment)) CoUninitialize();
            }).detach();
            return true;
        }
    }

    HMODULE current = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&Initialize), &current)) return false;
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(current, path, ARRAYSIZE(path))) return false;
    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!xaml) return false;
    auto initialize = reinterpret_cast<decltype(&InitializeXamlDiagnosticsEx)>(
        GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"));
    HRESULT hr = E_NOINTERFACE;
    if (initialize) {
        hr = initialize(L"VisualDiagConnection1", GetCurrentProcessId(), nullptr, path, bridgeClass, nullptr);
    }
    WriteDiagnostic("explorer", "InitializeXamlDiagnosticsEx hr=" + std::to_string(hr));
    FreeLibrary(xaml);
    return SUCCEEDED(hr);
}

void Lightency::XamlBridge::Shutdown() {
    enabled = false;
    std::lock_guard<std::mutex> lock(s_siteMutex);
    if (s_activeSite && s_activeSite->tree) {
        s_activeSite->tree->UnadviseVisualTreeChange(s_activeSite.Get());
    }
}
