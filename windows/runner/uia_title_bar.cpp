#include "uia_title_bar.h"

#include <oleauto.h>

namespace {

// Not in older SDK headers.
#ifndef WM_GETTITLEBARINFOEX
#define WM_GETTITLEBARINFOEX 0x033F
#endif

// The parts we describe. kRoot is the window itself: Windows already
// describes the window (name, position...), we only add children to it.
enum Part { kRoot, kTitleBar, kMinimize, kMaximize, kClose };

bool IsButton(Part part) {
  return part == kMinimize || part == kMaximize || part == kClose;
}

// One element of the tree. A single class plays every part; which
// interfaces it answers for depends on the part.
class Element : public IRawElementProviderSimple,
                public IRawElementProviderFragment,
                public IRawElementProviderFragmentRoot,
                public IInvokeProvider {
 public:
  // Root: Element(window, kRoot, nullptr). Others share the root.
  Element(HWND window, Part part, Element* root)
      : window_(window), part_(part), root_(root ? root : this) {
    if (root_ != this) {
      root_->AddRef();
    }
  }

  // --- IUnknown ------------------------------------------------------------

  IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) {
      return E_POINTER;
    }
    *out = nullptr;
    if (iid == __uuidof(IUnknown) ||
        iid == __uuidof(IRawElementProviderSimple)) {
      *out = static_cast<IRawElementProviderSimple*>(this);
    } else if (iid == __uuidof(IRawElementProviderFragment)) {
      *out = static_cast<IRawElementProviderFragment*>(this);
    } else if (iid == __uuidof(IRawElementProviderFragmentRoot) &&
               part_ == kRoot) {
      *out = static_cast<IRawElementProviderFragmentRoot*>(this);
    } else if (iid == __uuidof(IInvokeProvider) && IsButton(part_)) {
      *out = static_cast<IInvokeProvider*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  IFACEMETHODIMP_(ULONG) AddRef() override {
    return InterlockedIncrement(&references_);
  }

  IFACEMETHODIMP_(ULONG) Release() override {
    const ULONG count = InterlockedDecrement(&references_);
    if (count == 0) {
      delete this;
    }
    return count;
  }

  // --- IRawElementProviderSimple: what this element is ----------------------

  IFACEMETHODIMP get_ProviderOptions(ProviderOptions* out) override {
    *out = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider |
                                        ProviderOptions_UseComThreading);
    return S_OK;
  }

  IFACEMETHODIMP GetPatternProvider(PATTERNID pattern,
                                    IUnknown** out) override {
    *out = nullptr;
    // Buttons can be pressed ("invoked") by assistive technology.
    if (pattern == UIA_InvokePatternId && IsButton(part_)) {
      *out = static_cast<IInvokeProvider*>(this);
      AddRef();
    }
    return S_OK;
  }

  IFACEMETHODIMP GetPropertyValue(PROPERTYID property, VARIANT* out) override {
    VariantInit(out);
    if (!IsWindow(window_)) {
      return UIA_E_ELEMENTNOTAVAILABLE;
    }
    if (part_ == kRoot) {
      return S_OK;  // Windows' own description of the window is used.
    }
    switch (property) {
      case UIA_ControlTypePropertyId:
        out->vt = VT_I4;
        out->lVal =
            part_ == kTitleBar ? UIA_TitleBarControlTypeId
                               : UIA_ButtonControlTypeId;
        break;
      case UIA_NamePropertyId:
        out->vt = VT_BSTR;
        out->bstrVal = Name();
        break;
      case UIA_AutomationIdPropertyId:
        out->vt = VT_BSTR;
        out->bstrVal = SysAllocString(AutomationId());
        break;
      case UIA_IsKeyboardFocusablePropertyId:
        out->vt = VT_BOOL;
        out->boolVal = VARIANT_FALSE;  // like stock caption buttons
        break;
      case UIA_IsEnabledPropertyId:
        out->vt = VT_BOOL;
        out->boolVal = VARIANT_TRUE;
        break;
    }
    return S_OK;
  }

  IFACEMETHODIMP get_HostRawElementProvider(
      IRawElementProviderSimple** out) override {
    *out = nullptr;
    // The root stands for the window, so Windows merges our root with its
    // own description of the window (and its Flutter child window).
    if (part_ == kRoot) {
      return UiaHostProviderFromHwnd(window_, out);
    }
    return S_OK;
  }

  // --- IRawElementProviderFragment: where it is in the tree ----------------

  IFACEMETHODIMP Navigate(NavigateDirection direction,
                          IRawElementProviderFragment** out) override {
    *out = nullptr;
    if (!IsWindow(window_)) {
      return UIA_E_ELEMENTNOTAVAILABLE;
    }
    Part target = kRoot;
    bool found = false;
    switch (part_) {
      case kRoot:
        if (direction == NavigateDirection_FirstChild ||
            direction == NavigateDirection_LastChild) {
          target = kTitleBar;
          found = true;
        }
        break;
      case kTitleBar:
        if (direction == NavigateDirection_Parent) {
          *out = root_;
          root_->AddRef();
          return S_OK;
        }
        if (direction == NavigateDirection_FirstChild) {
          target = kMinimize;
          found = true;
        } else if (direction == NavigateDirection_LastChild) {
          target = kClose;
          found = true;
        }
        break;
      default:  // buttons, in order minimize, maximize, close
        if (direction == NavigateDirection_Parent) {
          target = kTitleBar;
          found = true;
        } else if (direction == NavigateDirection_NextSibling &&
                   part_ != kClose) {
          target = static_cast<Part>(part_ + 1);
          found = true;
        } else if (direction == NavigateDirection_PreviousSibling &&
                   part_ != kMinimize) {
          target = static_cast<Part>(part_ - 1);
          found = true;
        }
        break;
    }
    if (found) {
      *out = new Element(window_, target, root_);
    }
    return S_OK;
  }

  IFACEMETHODIMP GetRuntimeId(SAFEARRAY** out) override {
    *out = nullptr;
    if (part_ == kRoot) {
      return S_OK;  // Windows supplies the window's id.
    }
    // "Our window's id, then this part" - unique and stable.
    int ids[] = {UiaAppendRuntimeId, static_cast<int>(part_)};
    *out = SafeArrayCreateVector(VT_I4, 0, 2);
    if (!*out) {
      return E_OUTOFMEMORY;
    }
    for (LONG i = 0; i < 2; i++) {
      SafeArrayPutElement(*out, &i, &ids[i]);
    }
    return S_OK;
  }

  IFACEMETHODIMP get_BoundingRectangle(UiaRect* out) override {
    *out = {};
    if (!IsWindow(window_)) {
      return UIA_E_ELEMENTNOTAVAILABLE;
    }
    if (part_ == kRoot || IsIconic(window_) || !IsWindowVisible(window_)) {
      return S_OK;
    }
    // Ask the window (TitleBar answers with our layout, in screen pixels).
    TITLEBARINFOEX info = {sizeof(info)};
    SendMessageW(window_, WM_GETTITLEBARINFOEX, 0,
                 reinterpret_cast<LPARAM>(&info));
    RECT rect = info.rcTitleBar;
    if (part_ == kMinimize) {
      rect = info.rgrect[2];
    } else if (part_ == kMaximize) {
      rect = info.rgrect[3];
    } else if (part_ == kClose) {
      rect = info.rgrect[5];
    }
    *out = {static_cast<double>(rect.left), static_cast<double>(rect.top),
            static_cast<double>(rect.right - rect.left),
            static_cast<double>(rect.bottom - rect.top)};
    return S_OK;
  }

  IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
    *out = nullptr;
    return S_OK;
  }

  IFACEMETHODIMP SetFocus() override { return S_OK; }

  IFACEMETHODIMP get_FragmentRoot(
      IRawElementProviderFragmentRoot** out) override {
    *out = root_;
    root_->AddRef();
    return S_OK;
  }

  // --- IRawElementProviderFragmentRoot (root only): hit-testing ------------

  // What's under this screen point? This is how Narrator finds what the
  // mouse is over. Ask the window the same question Windows asks it.
  IFACEMETHODIMP ElementProviderFromPoint(
      double x, double y, IRawElementProviderFragment** out) override {
    *out = nullptr;
    if (!IsWindow(window_)) {
      return UIA_E_ELEMENTNOTAVAILABLE;
    }
    const LRESULT hit =
        SendMessageW(window_, WM_NCHITTEST, 0,
                     MAKELPARAM(static_cast<int>(x), static_cast<int>(y)));
    Part part = kRoot;
    switch (hit) {
      case HTMINBUTTON:
        part = kMinimize;
        break;
      case HTMAXBUTTON:
        part = kMaximize;
        break;
      case HTCLOSE:
        part = kClose;
        break;
      case HTCAPTION:
      case HTSYSMENU:
        part = kTitleBar;
        break;
    }
    if (part != kRoot) {
      *out = new Element(window_, part, root_);
    }
    return S_OK;  // nothing of ours there: the window itself
  }

  IFACEMETHODIMP GetFocus(IRawElementProviderFragment** out) override {
    *out = nullptr;
    return S_OK;
  }

  // --- IInvokeProvider (buttons only): press it ----------------------------

  IFACEMETHODIMP Invoke() override {
    if (!IsWindow(window_)) {
      return UIA_E_ELEMENTNOTAVAILABLE;
    }
    WPARAM command = SC_CLOSE;
    if (part_ == kMinimize) {
      command = SC_MINIMIZE;
    } else if (part_ == kMaximize) {
      command = IsZoomed(window_) ? SC_RESTORE : SC_MAXIMIZE;
    }
    // Posted, not sent: the screen reader shouldn't wait for it to finish.
    PostMessageW(window_, WM_SYSCOMMAND, command, 0);
    return S_OK;
  }

 private:
  ~Element() {
    if (root_ != this) {
      root_->Release();
    }
  }

  // Stock Windows names (English).
  BSTR Name() const {
    switch (part_) {
      case kMinimize:
        return SysAllocString(L"Minimize");
      case kMaximize:
        return SysAllocString(IsZoomed(window_) ? L"Restore" : L"Maximize");
      case kClose:
        return SysAllocString(L"Close");
      default: {
        // The title bar is named after the window title, like stock.
        const int length = GetWindowTextLengthW(window_);
        BSTR title = SysAllocStringLen(nullptr, length);
        if (title) {
          GetWindowTextW(window_, title, length + 1);
        }
        return title;
      }
    }
  }

  const wchar_t* AutomationId() const {
    switch (part_) {
      case kMinimize:
        return L"Minimize";
      case kMaximize:
        return L"Maximize";
      case kClose:
        return L"Close";
      default:
        return L"TitleBar";
    }
  }

  HWND window_;
  Part part_;
  Element* root_;  // the root element; |this| for the root itself
  ULONG references_ = 1;
};

}  // namespace

IRawElementProviderSimple* CreateTitleBarAccessibility(HWND window) {
  return new Element(window, kRoot, nullptr);
}
