/*
 *	IEnumMoniker implementation for DEVENUM.dll
 *
 * Copyright (C) 2002 Robert Shearman
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * NOTES ON THIS FILE:
 * - Implements IEnumMoniker interface which enumerates through moniker
 *   objects created from HKEY_CLASSES/CLSID/{DEVICE_CLSID}/Instance
 */

#include "devenum_private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(devenum);

typedef struct
{
    IEnumMoniker IEnumMoniker_iface;
    CLSID class;
    LONG ref;
    IEnumDMO *dmo_enum;
    HKEY sw_key;
    DWORD sw_index;
    HKEY cm_key;
    DWORD cm_index;
} EnumMonikerImpl;

static inline struct moniker *impl_from_IPropertyBag(IPropertyBag *iface)
{
    return CONTAINING_RECORD(iface, struct moniker, IPropertyBag_iface);
}

static HRESULT WINAPI property_bag_QueryInterface(IPropertyBag *iface, REFIID iid, void **out)
{
    struct moniker *moniker = impl_from_IPropertyBag(iface);

    TRACE("moniker %p, iid %s, out %p.\n", moniker, debugstr_guid(iid), out);

    if (!out)
        return E_POINTER;

    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IPropertyBag))
    {
        *out = iface;
        IPropertyBag_AddRef(iface);
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI property_bag_AddRef(IPropertyBag *iface)
{
    struct moniker *moniker = impl_from_IPropertyBag(iface);
    return IMoniker_AddRef(&moniker->IMoniker_iface);
}

static ULONG WINAPI property_bag_Release(IPropertyBag *iface)
{
    struct moniker *moniker = impl_from_IPropertyBag(iface);
    return IMoniker_Release(&moniker->IMoniker_iface);
}

static HRESULT WINAPI property_bag_Read(IPropertyBag *iface,
        const WCHAR *name, VARIANT *var, IErrorLog *errorlog)
{
    struct moniker *moniker = impl_from_IPropertyBag(iface);
    WCHAR dmo_name[80];
    DWORD size, type;
    HKEY parent, key;
    WCHAR path[78];
    void *data;
    HRESULT hr;
    LONG ret;

    TRACE("moniker %p, name %s, var %p, errorlog %p.\n", moniker, debugstr_w(name), var, errorlog);

    if (!name || !var)
        return E_POINTER;

    if (moniker->type == DEVICE_DMO)
    {
        if (!wcscmp(name, L"FriendlyName"))
        {
            if (SUCCEEDED(hr = DMOGetName(&moniker->clsid, dmo_name)))
            {
                V_VT(var) = VT_BSTR;
                V_BSTR(var) = SysAllocString(dmo_name);
            }
            return hr;
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    if (moniker->type == DEVICE_FILTER)
    {
        wcscpy(path, L"CLSID\\");
        if (moniker->has_class)
        {
            StringFromGUID2(&moniker->class, path + wcslen(path), CHARS_IN_GUID);
            wcscat(path, L"\\Instance");
        }
        if ((ret = RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, 0, &parent)))
            return HRESULT_FROM_WIN32(ret);
    }
    else if (moniker->type == DEVICE_CODEC)
    {
        wcscpy(path, L"Software\\Microsoft\\ActiveMovie\\devenum\\");
        if (moniker->has_class)
            StringFromGUID2(&moniker->class, path + wcslen(path), CHARS_IN_GUID);
        if ((ret = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, 0, &parent)))
            return HRESULT_FROM_WIN32(ret);
    }
    ret = RegOpenKeyExW(parent, moniker->name, 0, KEY_READ, &key);
    RegCloseKey(parent);
    if (ret)
        return HRESULT_FROM_WIN32(ret);

    if ((ret = RegQueryValueExW(key, name, NULL, NULL, NULL, &size)))
    {
        RegCloseKey(key);
        return HRESULT_FROM_WIN32(ret);
    }

    data = malloc(size);
    if ((ret = RegQueryValueExW(key, name, NULL, &type, data, &size)))
    {
        RegCloseKey(key);
        free(data);
        return HRESULT_FROM_WIN32(ret);
    }
    RegCloseKey(key);

    switch (type)
    {
    case REG_SZ:
        if (V_VT(var) == VT_EMPTY)
            V_VT(var) = VT_BSTR;
        if (V_VT(var) != VT_BSTR)
        {
            WARN("Invalid type %s.\n", debugstr_vt(V_VT(var)));
            return E_INVALIDARG;
        }
        V_BSTR(var) = SysAllocStringLen(data, size / sizeof(WCHAR) - 1);
        free(data);
        return S_OK;
    case REG_DWORD:
        if (V_VT(var) == VT_EMPTY)
            V_VT(var) = VT_I4;
        if (V_VT(var) != VT_I4)
        {
            WARN("Invalid type %s.\n", debugstr_vt(V_VT(var)));
            return E_INVALIDARG;
        }
        V_I4(var) = *(DWORD *)data;
        free(data);
        return S_OK;
    case REG_BINARY:
    {
        SAFEARRAYBOUND bound = {.cElements = size};
        void *array_data;

        if (V_VT(var) == VT_EMPTY)
            V_VT(var) = VT_ARRAY | VT_UI1;
        if (V_VT(var) != (VT_ARRAY | VT_UI1))
        {
            WARN("Invalid type %s.\n", debugstr_vt(V_VT(var)));
            return E_INVALIDARG;
        }

        if (!(V_ARRAY(var) = SafeArrayCreate(VT_UI1, 1, &bound)))
        {
            free(data);
            return E_OUTOFMEMORY;
        }

        SafeArrayAccessData(V_ARRAY(var), &array_data);
        memcpy(array_data, data, size);
        SafeArrayUnaccessData(V_ARRAY(var));
        free(data);
        return S_OK;
    }
    default:
        FIXME("Unhandled type %#x.\n", type);
        free(data);
        return E_NOTIMPL;
    }
}

static HRESULT WINAPI property_bag_Write(IPropertyBag *iface, const WCHAR *name, VARIANT *var)
{
    struct moniker *moniker = impl_from_IPropertyBag(iface);
    HKEY parent, key;
    WCHAR path[78];
    LONG ret;

    TRACE("moniker %p, name %s, var %s.\n", moniker, debugstr_w(name), debugstr_variant(var));

    if (moniker->type == DEVICE_DMO)
        return E_ACCESSDENIED;

    if (moniker->type == DEVICE_FILTER)
    {
        wcscpy(path, L"CLSID\\");
        if (moniker->has_class)
        {
            StringFromGUID2(&moniker->class, path + wcslen(path), CHARS_IN_GUID);
            wcscat(path, L"\\Instance");
        }
        if ((ret = RegCreateKeyExW(HKEY_CLASSES_ROOT, path, 0, NULL, 0, 0, NULL, &parent, NULL)))
            return HRESULT_FROM_WIN32(ret);
    }
    else if (moniker->type == DEVICE_CODEC)
    {
        wcscpy(path, L"Software\\Microsoft\\ActiveMovie\\devenum\\");
        if (moniker->has_class)
            StringFromGUID2(&moniker->class, path + wcslen(path), CHARS_IN_GUID);
        if ((ret = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, NULL, 0, 0, NULL, &parent, NULL)))
            return HRESULT_FROM_WIN32(ret);
    }
    ret = RegCreateKeyExW(parent, moniker->name, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
    RegCloseKey(parent);
    if (ret)
        return HRESULT_FROM_WIN32(ret);

    switch (V_VT(var))
    {
    case VT_BSTR:
        ret = RegSetValueExW(key, name, 0, REG_SZ, (BYTE *)V_BSTR(var),
                (wcslen(V_BSTR(var)) + 1) * sizeof(WCHAR));
        break;
    case VT_I4:
        ret = RegSetValueExW(key, name, 0, REG_DWORD, (BYTE *)&V_I4(var), sizeof(DWORD));
        break;
    case VT_ARRAY | VT_UI1:
    {
        LONG lbound, ubound;
        void *array_data;
        SafeArrayGetLBound(V_ARRAY(var), 1, &lbound);
        SafeArrayGetUBound(V_ARRAY(var), 1, &ubound);
        SafeArrayAccessData(V_ARRAY(var), &array_data);
        ret = RegSetValueExW(key, name, 0, REG_BINARY, array_data, ubound - lbound + 1);
        SafeArrayUnaccessData(V_ARRAY(var));
        break;
    }
    default:
        WARN("Unhandled type %s.\n", debugstr_vt(V_VT(var)));
        return E_INVALIDARG;
    }

    RegCloseKey(key);
    return S_OK;
}

static const IPropertyBagVtbl IPropertyBag_Vtbl =
{
    property_bag_QueryInterface,
    property_bag_AddRef,
    property_bag_Release,
    property_bag_Read,
    property_bag_Write,
};

static inline struct moniker *impl_from_IMoniker(IMoniker *iface)
{
    return CONTAINING_RECORD(iface, struct moniker, IMoniker_iface);
}

static HRESULT WINAPI moniker_QueryInterface(IMoniker *iface, REFIID riid, void **ppv)
{
    TRACE("\n\tIID:\t%s\n",debugstr_guid(riid));

    if (!ppv)
        return E_POINTER;

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IPersist) ||
        IsEqualGUID(riid, &IID_IPersistStream) ||
        IsEqualGUID(riid, &IID_IMoniker))
    {
        *ppv = iface;
        IMoniker_AddRef(iface);
        return S_OK;
    }

    FIXME("- no interface IID: %s\n", debugstr_guid(riid));
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI moniker_AddRef(IMoniker *iface)
{
    struct moniker *This = impl_from_IMoniker(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%d\n", This, ref);

    return ref;
}

static ULONG WINAPI moniker_Release(IMoniker *iface)
{
    struct moniker *This = impl_from_IMoniker(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%d\n", This, ref);

    if (ref == 0) {
        CoTaskMemFree(This->name);
        free(This);
        DEVENUM_UnlockModule();
    }
    return ref;
}

static HRESULT WINAPI moniker_GetClassID(IMoniker *iface, CLSID *pClassID)
{
    struct moniker *This = impl_from_IMoniker(iface);

    TRACE("(%p)->(%p)\n", This, pClassID);

    if (pClassID == NULL)
        return E_INVALIDARG;

    *pClassID = CLSID_CDeviceMoniker;

    return S_OK;
}

static HRESULT WINAPI moniker_IsDirty(IMoniker *iface)
{
    FIXME("(%p)->(): stub\n", iface);

    return S_FALSE;
}

static HRESULT WINAPI moniker_Load(IMoniker *iface, IStream *pStm)
{
    FIXME("(%p)->(%p): stub\n", iface, pStm);

    return E_NOTIMPL;
}

static HRESULT WINAPI moniker_Save(IMoniker *iface, IStream *pStm, BOOL fClearDirty)
{
    FIXME("(%p)->(%p, %s): stub\n", iface, pStm, fClearDirty ? "true" : "false");

    return STG_E_CANTSAVE;
}

static HRESULT WINAPI moniker_GetSizeMax(IMoniker *iface, ULARGE_INTEGER *pcbSize)
{
    FIXME("(%p)->(%p): stub\n", iface, pcbSize);

    ZeroMemory(pcbSize, sizeof(*pcbSize));

    return S_OK;
}

static HRESULT WINAPI moniker_BindToObject(IMoniker *iface, IBindCtx *bind_ctx,
        IMoniker *left, REFIID iid, void **out)
{
    struct moniker *moniker = impl_from_IMoniker(iface);
    IPersistPropertyBag *persist_bag;
    IUnknown *unk;
    CLSID clsid;
    VARIANT var;
    HRESULT hr;

    TRACE("moniker %p, bind_ctx %p, left %p, iid %s, out %p.\n",
            moniker, bind_ctx, left, debugstr_guid(iid), out);

    if (!out)
        return E_POINTER;

    *out = NULL;

    if (moniker->type == DEVICE_DMO)
    {
        IDMOWrapperFilter *wrapper;

        if (FAILED(hr = CoCreateInstance(&CLSID_DMOWrapperFilter, NULL,
                CLSCTX_INPROC_SERVER, &IID_IDMOWrapperFilter, (void **)&wrapper)))
            return hr;

        if (SUCCEEDED(hr = IDMOWrapperFilter_Init(wrapper, &moniker->clsid, &moniker->class)))
        {
            hr = IDMOWrapperFilter_QueryInterface(wrapper, iid, out);
        }
        IDMOWrapperFilter_Release(wrapper);
        return hr;
    }

    VariantInit(&var);
    V_VT(&var) = VT_BSTR;
    if (FAILED(hr = IPropertyBag_Read(&moniker->IPropertyBag_iface, L"CLSID", &var, NULL)))
        return hr;

    hr = CLSIDFromString(V_BSTR(&var), &clsid);
    VariantClear(&var);
    if (FAILED(hr))
        return hr;

    if (FAILED(hr = CoCreateInstance(&clsid, NULL, CLSCTX_ALL, &IID_IUnknown, (void **)&unk)))
        return hr;

    if (SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IPersistPropertyBag, (void **)&persist_bag)))
    {
        hr = IPersistPropertyBag_Load(persist_bag, &moniker->IPropertyBag_iface, NULL);
        IPersistPropertyBag_Release(persist_bag);
    }

    if (SUCCEEDED(hr))
        hr = IUnknown_QueryInterface(unk, iid, out);

    IUnknown_Release(unk);

    return hr;
}

static HRESULT WINAPI moniker_BindToStorage(IMoniker *iface, IBindCtx *pbc,
        IMoniker *pmkToLeft, REFIID riid, void **out)
{
    struct moniker *moniker = impl_from_IMoniker(iface);

    TRACE("moniker %p, left %p, iid %s, out %p.\n", moniker, pmkToLeft, debugstr_guid(riid), out);

    *out = NULL;

    if (pmkToLeft)
        return MK_E_NOSTORAGE;

    if (pbc != NULL)
    {
        static DWORD reported;
        if (!reported)
        {
            FIXME("ignoring IBindCtx %p\n", pbc);
            reported++;
        }
    }

    if (IsEqualGUID(riid, &IID_IPropertyBag))
    {
        *out = &moniker->IPropertyBag_iface;
        IPropertyBag_AddRef(&moniker->IPropertyBag_iface);
        return S_OK;
    }

    return MK_E_NOSTORAGE;
}

static HRESULT WINAPI moniker_Reduce(IMoniker *iface, IBindCtx *pbc,
        DWORD dwReduceHowFar, IMoniker **ppmkToLeft, IMoniker **ppmkReduced)
{
    TRACE("(%p)->(%p, %d, %p, %p)\n", iface, pbc, dwReduceHowFar, ppmkToLeft, ppmkReduced);

    if (ppmkToLeft)
        *ppmkToLeft = NULL;
    *ppmkReduced = iface;

    return MK_S_REDUCED_TO_SELF;
}

static HRESULT WINAPI moniker_ComposeWith(IMoniker *iface, IMoniker *pmkRight,
        BOOL fOnlyIfNotGeneric, IMoniker **ppmkComposite)
{
    FIXME("(%p)->(%p, %s, %p): stub\n", iface, pmkRight, fOnlyIfNotGeneric ? "true" : "false", ppmkComposite);

    /* FIXME: use CreateGenericComposite? */
    *ppmkComposite = NULL;

    return E_NOTIMPL;
}

static HRESULT WINAPI moniker_Enum(IMoniker *iface, BOOL fForward,
        IEnumMoniker **ppenumMoniker)
{
    FIXME("(%p)->(%s, %p): stub\n", iface, fForward ? "true" : "false", ppenumMoniker);

    *ppenumMoniker = NULL;

    return S_OK;
}

static HRESULT WINAPI moniker_IsEqual(IMoniker *iface, IMoniker *pmkOtherMoniker)
{
    CLSID clsid;
    LPOLESTR this_name, other_name;
    IBindCtx *bind;
    HRESULT res;

    TRACE("(%p)->(%p)\n", iface, pmkOtherMoniker);

    if (!pmkOtherMoniker)
        return E_INVALIDARG;

    IMoniker_GetClassID(pmkOtherMoniker, &clsid);
    if (!IsEqualCLSID(&clsid, &CLSID_CDeviceMoniker))
        return S_FALSE;

    res = CreateBindCtx(0, &bind);
    if (FAILED(res))
       return res;

    res = S_FALSE;
    if (SUCCEEDED(IMoniker_GetDisplayName(iface, bind, NULL, &this_name)) &&
        SUCCEEDED(IMoniker_GetDisplayName(pmkOtherMoniker, bind, NULL, &other_name)))
    {
        int result = lstrcmpiW(this_name, other_name);
        CoTaskMemFree(this_name);
        CoTaskMemFree(other_name);
        if (!result)
            res = S_OK;
    }
    IBindCtx_Release(bind);
    return res;
}

static HRESULT WINAPI moniker_Hash(IMoniker *iface, DWORD *pdwHash)
{
    TRACE("(%p)->(%p)\n", iface, pdwHash);

    *pdwHash = 0;

    return S_OK;
}

static HRESULT WINAPI moniker_IsRunning(IMoniker *iface, IBindCtx *pbc,
        IMoniker *pmkToLeft, IMoniker *pmkNewlyRunning)
{
    FIXME("(%p)->(%p, %p, %p): stub\n", iface, pbc, pmkToLeft, pmkNewlyRunning);

    return S_FALSE;
}

static HRESULT WINAPI moniker_GetTimeOfLastChange(IMoniker *iface, IBindCtx *pbc,
        IMoniker *pmkToLeft, FILETIME *pFileTime)
{
    TRACE("(%p)->(%p, %p, %p)\n", iface, pbc, pmkToLeft, pFileTime);

    pFileTime->dwLowDateTime = 0xFFFFFFFF;
    pFileTime->dwHighDateTime = 0x7FFFFFFF;

    return MK_E_UNAVAILABLE;
}

static HRESULT WINAPI moniker_Inverse(IMoniker *iface, IMoniker **ppmk)
{
    TRACE("(%p)->(%p)\n", iface, ppmk);

    *ppmk = NULL;

    return MK_E_NOINVERSE;
}

static HRESULT WINAPI moniker_CommonPrefixWith(IMoniker *iface,
        IMoniker *pmkOtherMoniker, IMoniker **ppmkPrefix)
{
    TRACE("(%p)->(%p, %p)\n", iface, pmkOtherMoniker, ppmkPrefix);

    *ppmkPrefix = NULL;

    return MK_E_NOPREFIX;
}

static HRESULT WINAPI moniker_RelativePathTo(IMoniker *iface, IMoniker *pmkOther,
        IMoniker **ppmkRelPath)
{
    TRACE("(%p)->(%p, %p)\n", iface, pmkOther, ppmkRelPath);

    *ppmkRelPath = pmkOther;

    return MK_S_HIM;
}

static HRESULT WINAPI moniker_GetDisplayName(IMoniker *iface, IBindCtx *pbc,
        IMoniker *pmkToLeft, LPOLESTR *ppszDisplayName)
{
    struct moniker *This = impl_from_IMoniker(iface);
    WCHAR *buffer;

    TRACE("(%p)->(%p, %p, %p)\n", iface, pbc, pmkToLeft, ppszDisplayName);

    *ppszDisplayName = NULL;

    if (This->type == DEVICE_DMO)
    {
        buffer = CoTaskMemAlloc((lstrlenW(deviceW) + lstrlenW(dmoW)
                                 + 2 * CHARS_IN_GUID + 1) * sizeof(WCHAR));
        if (!buffer) return E_OUTOFMEMORY;

        lstrcpyW(buffer, deviceW);
        lstrcatW(buffer, dmoW);
        StringFromGUID2(&This->clsid, buffer + lstrlenW(buffer), CHARS_IN_GUID);
        StringFromGUID2(&This->class, buffer + lstrlenW(buffer), CHARS_IN_GUID);
    }
    else
    {
        buffer = CoTaskMemAlloc((lstrlenW(deviceW) + 3 + (This->has_class ? CHARS_IN_GUID : 0)
                                 + lstrlenW(This->name) + 1) * sizeof(WCHAR));
        if (!buffer) return E_OUTOFMEMORY;

        lstrcpyW(buffer, deviceW);
        if (This->type == DEVICE_FILTER)
            lstrcatW(buffer, swW);
        else if (This->type == DEVICE_CODEC)
            lstrcatW(buffer, cmW);

        if (This->has_class)
        {
            StringFromGUID2(&This->class, buffer + lstrlenW(buffer), CHARS_IN_GUID);
            lstrcatW(buffer, backslashW);
        }
        lstrcatW(buffer, This->name);
    }

    *ppszDisplayName = buffer;
    return S_OK;
}

static HRESULT WINAPI moniker_ParseDisplayName(IMoniker *iface, IBindCtx *pbc,
        IMoniker *pmkToLeft, LPOLESTR pszDisplayName, ULONG *pchEaten, IMoniker **ppmkOut)
{
    FIXME("(%p)->(%p, %p, %s, %p, %p)\n", iface, pbc, pmkToLeft, debugstr_w(pszDisplayName), pchEaten, ppmkOut);

    *pchEaten = 0;
    *ppmkOut = NULL;

    return MK_E_SYNTAX;
}

static HRESULT WINAPI moniker_IsSystemMoniker(IMoniker *iface, DWORD *pdwMksys)
{
    TRACE("(%p)->(%p)\n", iface, pdwMksys);

    return S_FALSE;
}

static const IMonikerVtbl IMoniker_Vtbl =
{
    moniker_QueryInterface,
    moniker_AddRef,
    moniker_Release,
    moniker_GetClassID,
    moniker_IsDirty,
    moniker_Load,
    moniker_Save,
    moniker_GetSizeMax,
    moniker_BindToObject,
    moniker_BindToStorage,
    moniker_Reduce,
    moniker_ComposeWith,
    moniker_Enum,
    moniker_IsEqual,
    moniker_Hash,
    moniker_IsRunning,
    moniker_GetTimeOfLastChange,
    moniker_Inverse,
    moniker_CommonPrefixWith,
    moniker_RelativePathTo,
    moniker_GetDisplayName,
    moniker_ParseDisplayName,
    moniker_IsSystemMoniker,
};

struct moniker *filter_moniker_create(const GUID *class, const WCHAR *name)
{
    struct moniker *object;

    if (!(object = calloc(1, sizeof(*object))))
        return NULL;

    object->IMoniker_iface.lpVtbl = &IMoniker_Vtbl;
    object->IPropertyBag_iface.lpVtbl = &IPropertyBag_Vtbl;
    object->ref = 1;
    object->type = DEVICE_FILTER;
    if (class)
        object->class = *class;
    object->has_class = !!class;
    object->name = wcsdup(name);

    DEVENUM_LockModule();

    return object;
}

struct moniker *codec_moniker_create(const GUID *class, const WCHAR *name)
{
    struct moniker *object;

    if (!(object = calloc(1, sizeof(*object))))
        return NULL;

    object->IMoniker_iface.lpVtbl = &IMoniker_Vtbl;
    object->IPropertyBag_iface.lpVtbl = &IPropertyBag_Vtbl;
    object->ref = 1;
    object->type = DEVICE_CODEC;
    if (class)
        object->class = *class;
    object->has_class = !!class;
    object->name = wcsdup(name);

    DEVENUM_LockModule();

    return object;
}

struct moniker *dmo_moniker_create(const GUID class, const GUID clsid)
{
    struct moniker *object;

    if (!(object = calloc(1, sizeof(*object))))
        return NULL;

    object->IMoniker_iface.lpVtbl = &IMoniker_Vtbl;
    object->IPropertyBag_iface.lpVtbl = &IPropertyBag_Vtbl;
    object->ref = 1;
    object->type = DEVICE_DMO;
    object->class = class;
    object->clsid = clsid;

    DEVENUM_LockModule();

    return object;
}

static inline EnumMonikerImpl *impl_from_IEnumMoniker(IEnumMoniker *iface)
{
    return CONTAINING_RECORD(iface, EnumMonikerImpl, IEnumMoniker_iface);
}

static HRESULT WINAPI enum_moniker_QueryInterface(IEnumMoniker *iface, REFIID riid, void **ppv)
{
    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IEnumMoniker))
    {
        *ppv = iface;
        IEnumMoniker_AddRef(iface);
        return S_OK;
    }

    FIXME("- no interface IID: %s\n", debugstr_guid(riid));
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI enum_moniker_AddRef(IEnumMoniker *iface)
{
    EnumMonikerImpl *This = impl_from_IEnumMoniker(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%d\n", This, ref);

    return ref;
}

static ULONG WINAPI enum_moniker_Release(IEnumMoniker *iface)
{
    EnumMonikerImpl *This = impl_from_IEnumMoniker(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%d\n", This, ref);

    if (!ref)
    {
        IEnumDMO_Release(This->dmo_enum);
        RegCloseKey(This->sw_key);
        RegCloseKey(This->cm_key);
        CoTaskMemFree(This);
        DEVENUM_UnlockModule();
        return 0;
    }
    return ref;
}

static HRESULT WINAPI enum_moniker_Next(IEnumMoniker *iface, ULONG celt, IMoniker **rgelt,
        ULONG *pceltFetched)
{
    EnumMonikerImpl *This = impl_from_IEnumMoniker(iface);
    WCHAR buffer[MAX_PATH + 1];
    struct moniker *moniker;
    LONG res;
    ULONG fetched = 0;
    HRESULT hr;
    GUID clsid;
    HKEY hkey;

    TRACE("(%p)->(%d, %p, %p)\n", iface, celt, rgelt, pceltFetched);

    while (fetched < celt)
    {
        /* FIXME: try PNP devices first */

        /* try DMOs */
        if ((hr = IEnumDMO_Next(This->dmo_enum, 1, &clsid, NULL, NULL)) == S_OK)
        {
            moniker = dmo_moniker_create(This->class, clsid);
        }
        /* try DirectShow filters */
        else if (!(res = RegEnumKeyW(This->sw_key, This->sw_index, buffer, ARRAY_SIZE(buffer))))
        {
            This->sw_index++;
            if ((res = RegOpenKeyExW(This->sw_key, buffer, 0, KEY_QUERY_VALUE, &hkey)))
                break;

            moniker = filter_moniker_create(&This->class, buffer);
        }
        /* then try codecs */
        else if (!(res = RegEnumKeyW(This->cm_key, This->cm_index, buffer, ARRAY_SIZE(buffer))))
        {
            This->cm_index++;

            if ((res = RegOpenKeyExW(This->cm_key, buffer, 0, KEY_QUERY_VALUE, &hkey)))
                break;

            moniker = codec_moniker_create(&This->class, buffer);
        }
        else
            break;

        if (!moniker)
            return E_OUTOFMEMORY;

        rgelt[fetched++] = &moniker->IMoniker_iface;
    }

    TRACE("-- fetched %d\n", fetched);

    if (pceltFetched)
        *pceltFetched = fetched;

    if (fetched != celt)
        return S_FALSE;
    else
        return S_OK;
}

static HRESULT WINAPI enum_moniker_Skip(IEnumMoniker *iface, ULONG celt)
{
    EnumMonikerImpl *This = impl_from_IEnumMoniker(iface);

    TRACE("(%p)->(%d)\n", iface, celt);

    while (celt--)
    {
        /* FIXME: try PNP devices first */

        /* try DMOs */
        if (IEnumDMO_Skip(This->dmo_enum, 1) == S_OK)
            ;
        /* try DirectShow filters */
        else if (RegEnumKeyW(This->sw_key, This->sw_index, NULL, 0) != ERROR_NO_MORE_ITEMS)
        {
            This->sw_index++;
        }
        /* then try codecs */
        else if (RegEnumKeyW(This->cm_key, This->cm_index, NULL, 0) != ERROR_NO_MORE_ITEMS)
        {
            This->cm_index++;
        }
        else
            return S_FALSE;
    }

    return S_OK;
}

static HRESULT WINAPI enum_moniker_Reset(IEnumMoniker *iface)
{
    EnumMonikerImpl *This = impl_from_IEnumMoniker(iface);

    TRACE("(%p)->()\n", iface);

    IEnumDMO_Reset(This->dmo_enum);
    This->sw_index = 0;
    This->cm_index = 0;

    return S_OK;
}

static HRESULT WINAPI enum_moniker_Clone(IEnumMoniker *iface, IEnumMoniker **ppenum)
{
    FIXME("(%p)->(%p): stub\n", iface, ppenum);

    return E_NOTIMPL;
}

/**********************************************************************
 * IEnumMoniker_Vtbl
 */
static const IEnumMonikerVtbl IEnumMoniker_Vtbl =
{
    enum_moniker_QueryInterface,
    enum_moniker_AddRef,
    enum_moniker_Release,
    enum_moniker_Next,
    enum_moniker_Skip,
    enum_moniker_Reset,
    enum_moniker_Clone,
};

HRESULT enum_moniker_create(REFCLSID class, IEnumMoniker **ppEnumMoniker)
{
    EnumMonikerImpl * pEnumMoniker = CoTaskMemAlloc(sizeof(EnumMonikerImpl));
    WCHAR buffer[78];
    HRESULT hr;

    if (!pEnumMoniker)
        return E_OUTOFMEMORY;

    pEnumMoniker->IEnumMoniker_iface.lpVtbl = &IEnumMoniker_Vtbl;
    pEnumMoniker->ref = 1;
    pEnumMoniker->sw_index = 0;
    pEnumMoniker->cm_index = 0;
    pEnumMoniker->class = *class;

    lstrcpyW(buffer, clsidW);
    lstrcatW(buffer, backslashW);
    StringFromGUID2(class, buffer + lstrlenW(buffer), CHARS_IN_GUID);
    lstrcatW(buffer, instanceW);
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, buffer, 0, KEY_ENUMERATE_SUB_KEYS, &pEnumMoniker->sw_key))
        pEnumMoniker->sw_key = NULL;

    lstrcpyW(buffer, wszActiveMovieKey);
    StringFromGUID2(class, buffer + lstrlenW(buffer), CHARS_IN_GUID);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, buffer, 0, KEY_ENUMERATE_SUB_KEYS, &pEnumMoniker->cm_key))
        pEnumMoniker->cm_key = NULL;

    hr = DMOEnum(class, 0, 0, NULL, 0, NULL, &pEnumMoniker->dmo_enum);
    if (FAILED(hr))
    {
        IEnumMoniker_Release(&pEnumMoniker->IEnumMoniker_iface);
        return hr;
    }

    *ppEnumMoniker = &pEnumMoniker->IEnumMoniker_iface;

    DEVENUM_LockModule();

    return S_OK;
}
