/*
 * ReactOS Explorer
 *
 * Copyright 2006 - 2007 Thomas Weidenmueller <w3seek@reactos.org>
 * Copyright 2018 Ged Murphy <gedmurphy@reactos.org>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "precomp.h"

// Data comes from shell32/systray.cpp -> TrayNotifyCDS_Dummy
typedef struct _SYS_PAGER_COPY_DATA
{
    DWORD           cookie;
    DWORD           notify_code;
    NOTIFYICONDATA  nicon_data;
} SYS_PAGER_COPY_DATA, *PSYS_PAGER_COPY_DATA;

CIconWatcher::CIconWatcher() :
    m_hWatcherThread(NULL),
    m_WakeUpEvent(NULL),
    m_hwndSysTray(NULL),
    m_Loop(false)
{
}

CIconWatcher::~CIconWatcher()
{
    Uninitialize();
    DeleteCriticalSection(&m_ListLock);

    if (m_WakeUpEvent)
        CloseHandle(m_WakeUpEvent);
    if (m_hWatcherThread)
        CloseHandle(m_hWatcherThread);
}

bool CIconWatcher::Initialize(_In_ HWND hWndParent)
{
    m_hwndSysTray = hWndParent;

    InitializeCriticalSection(&m_ListLock);
    m_WakeUpEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (m_WakeUpEvent == NULL)
        return false;

    m_hWatcherThread = (HANDLE)_beginthreadex(NULL,
                                                0,
                                                WatcherThread,
                                                (LPVOID)this,
                                                0,
                                                NULL);
    if (m_hWatcherThread == NULL)
        return false;

    return true;
}

void CIconWatcher::Uninitialize()
{
    m_Loop = false;
    if (m_WakeUpEvent)
        SetEvent(m_WakeUpEvent);

    EnterCriticalSection(&m_ListLock);

    POSITION Pos;
    for (size_t i = 0; i < m_WatcherList.GetCount(); i++)
    {
        Pos = m_WatcherList.FindIndex(i);
        if (Pos)
        {
            IconWatcherData *Icon;
            Icon = m_WatcherList.GetAt(Pos);
            delete Icon;
        }
    }
    m_WatcherList.RemoveAll();

    LeaveCriticalSection(&m_ListLock);
}

bool CIconWatcher::AddIconToWatcher(_In_ NOTIFYICONDATA *iconData)
{
    DWORD ProcessId;
    (void)GetWindowThreadProcessId(iconData->hWnd, &ProcessId);

    HANDLE hProcess;
    hProcess = OpenProcess(SYNCHRONIZE, FALSE, ProcessId);
    if (hProcess == NULL)
    {
        return false;
    }

    IconWatcherData *Icon = new IconWatcherData(iconData);
    Icon->hProcess = hProcess;
    Icon->ProcessId;

    bool Added = false;
    EnterCriticalSection(&m_ListLock);

    // The likelyhood of someone having more than 64 icons in their tray is
    // pretty slim. We could spin up a new thread for each multiple of 64, but
    // it's not worth the effort, so we just won't bother watching those icons
    if (m_WatcherList.GetCount() < MAXIMUM_WAIT_OBJECTS)
    {
        m_WatcherList.AddTail(Icon);
        SetEvent(m_WakeUpEvent);
        Added = true;
    }

    LeaveCriticalSection(&m_ListLock);

    if (!Added)
    {
        delete Icon;
    }

    return Added;
}

bool CIconWatcher::RemoveIconFromWatcher(_In_ NOTIFYICONDATA *iconData)
{
    EnterCriticalSection(&m_ListLock);
        
    IconWatcherData *Icon;
    Icon = GetListEntry(iconData, NULL, true);

    SetEvent(m_WakeUpEvent);
    LeaveCriticalSection(&m_ListLock);

    delete Icon;
    return true;
}

IconWatcherData* CIconWatcher::GetListEntry(_In_opt_ NOTIFYICONDATA *iconData, _In_opt_ HANDLE hProcess, _In_ bool Remove)
{
    IconWatcherData *Entry = NULL;
    POSITION NextPosition = m_WatcherList.GetHeadPosition();
    POSITION Position;
    do
    {
        Position = NextPosition;

        Entry = m_WatcherList.GetNext(NextPosition);
        if (Entry)
        {
            if ((iconData && ((Entry->IconData.hWnd == iconData->hWnd) && (Entry->IconData.uID == iconData->uID))) ||
                    (hProcess && (Entry->hProcess == hProcess)))
            {
                if (Remove)
                    m_WatcherList.RemoveAt(Position);
                break;
            }
        }
        Entry = NULL;

    } while (NextPosition != NULL);

    return Entry;
}

UINT WINAPI CIconWatcher::WatcherThread(_In_opt_ LPVOID lpParam)
{
    CIconWatcher* This = reinterpret_cast<CIconWatcher *>(lpParam);
    HANDLE *WatchList = NULL;

    This->m_Loop = true;
    while (This->m_Loop)
    {
        EnterCriticalSection(&This->m_ListLock);

        DWORD Size;
        Size = This->m_WatcherList.GetCount() + 1;
        ASSERT(Size <= MAXIMUM_WAIT_OBJECTS);

        if (WatchList)
            delete WatchList;
        WatchList = new HANDLE[Size];
        WatchList[0] = This->m_WakeUpEvent;

        POSITION Pos;
        for (size_t i = 0; i < This->m_WatcherList.GetCount(); i++)
        {
            Pos = This->m_WatcherList.FindIndex(i);
            if (Pos)
            {
                IconWatcherData *Icon;
                Icon = This->m_WatcherList.GetAt(Pos);
                WatchList[i + 1] = Icon->hProcess;
            }
        }

        LeaveCriticalSection(&This->m_ListLock);

        DWORD Status;
        Status = WaitForMultipleObjects(Size,
                                        WatchList,
                                        FALSE,
                                        INFINITE);
        if (Status == WAIT_OBJECT_0)
        {
            // We've been kicked, we have updates to our list (or we're exiting the thread)
            if (This->m_Loop)
                TRACE("Updating watched icon list");
        }
        else if ((Status >= WAIT_OBJECT_0 + 1) && (Status < Size))
        {
            IconWatcherData *Icon;
            Icon = This->GetListEntry(NULL, WatchList[Status], false);

            TRACE("Pid %lu owns a notification icon and has stopped without deleting it. We'll cleanup on its behalf", Icon->ProcessId);

            int len = FIELD_OFFSET(SYS_PAGER_COPY_DATA, nicon_data) + Icon->IconData.cbSize;
            PSYS_PAGER_COPY_DATA pnotify_data = (PSYS_PAGER_COPY_DATA)new BYTE[len];
            pnotify_data->cookie = 1;
            pnotify_data->notify_code = NIM_DELETE;
            memcpy(&pnotify_data->nicon_data, &Icon->IconData, Icon->IconData.cbSize);

            COPYDATASTRUCT data;
            data.dwData = 1;
            data.cbData = len;
            data.lpData = pnotify_data;

            BOOL Success = FALSE;
            HWND parentHWND = ::GetParent(GetParent(This->m_hwndSysTray));
            if (parentHWND)
                Success = ::SendMessage(parentHWND, WM_COPYDATA, (WPARAM)&Icon->IconData, (LPARAM)&data);

            delete pnotify_data;

            if (!Success)
            {
                // If we failed to handle the delete message, forcibly remove it
                This->RemoveIconFromWatcher(&Icon->IconData);
            }
        }
        else
        {
            if (Status == WAIT_FAILED)
            {
                Status = GetLastError();
            }
            ERR("Failed to wait on process handles : %lu\n", Status);
            This->Uninitialize();
        }
    }

    if (WatchList)
        delete WatchList;

    return 0;
}

/*
* NotifyToolbar
*/

CBalloonQueue::CBalloonQueue() :
    m_hwndParent(NULL),
    m_tooltips(NULL),
    m_toolbar(NULL),
    m_current(NULL),
    m_currentClosed(false),
    m_timer(-1)
{
}

void CBalloonQueue::Init(HWND hwndParent, CToolbar<InternalIconData> * toolbar, CTooltips * balloons)
{
    m_hwndParent = hwndParent;
    m_toolbar = toolbar;
    m_tooltips = balloons;
}

void CBalloonQueue::Deinit()
{
    if (m_timer >= 0)
    {
        ::KillTimer(m_hwndParent, m_timer);
    }
}

bool CBalloonQueue::OnTimer(int timerId)
{
    if (timerId != m_timer)
        return false;

    ::KillTimer(m_hwndParent, m_timer);
    m_timer = -1;

    if (m_current && !m_currentClosed)
    {
        Close(m_current);
    }
    else
    {
        m_current = NULL;
        m_currentClosed = false;
        if (!m_queue.IsEmpty())
        {
            Info info = m_queue.RemoveHead();
            Show(info);
        }
    }

    return true;
}

void CBalloonQueue::UpdateInfo(InternalIconData * notifyItem)
{
    size_t len = 0;
    HRESULT hr = StringCchLength(notifyItem->szInfo, _countof(notifyItem->szInfo), &len);
    if (SUCCEEDED(hr) && len > 0)
    {
        Info info(notifyItem);

        // If m_current == notifyItem, we want to replace the previous balloon even if there is a queue.
        if (m_current != notifyItem && (m_current != NULL || !m_queue.IsEmpty()))
        {
            m_queue.AddTail(info);
        }
        else
        {
            Show(info);
        }
    }
    else
    {
        Close(notifyItem);
    }
}

void CBalloonQueue::RemoveInfo(InternalIconData * notifyItem)
{
    Close(notifyItem);

    POSITION position = m_queue.GetHeadPosition();
    while(position != NULL)
    {
        Info& info = m_queue.GetNext(position);
        if (info.pSource == notifyItem)
        {
            m_queue.RemoveAt(position);
        }
    }
}

void CBalloonQueue::CloseCurrent()
{
    if (m_current != NULL)
        Close(m_current);
}

int CBalloonQueue::IndexOf(InternalIconData * pdata)
{
    int count = m_toolbar->GetButtonCount();
    for (int i = 0; i < count; i++)
    {
        if (m_toolbar->GetItemData(i) == pdata)
            return i;
    }
    return -1;
}

void CBalloonQueue::SetTimer(int length)
{
    m_timer = ::SetTimer(m_hwndParent, BalloonsTimerId, length, NULL);
}

void CBalloonQueue::Show(Info& info)
{
    TRACE("ShowBalloonTip called for flags=%x text=%ws; title=%ws\n", info.uIcon, info.szInfo, info.szInfoTitle);

    // TODO: NIF_REALTIME, NIIF_NOSOUND, other Vista+ flags

    const int index = IndexOf(info.pSource);
    RECT rc;
    m_toolbar->GetItemRect(index, &rc);
    m_toolbar->ClientToScreen(&rc);
    const WORD x = (rc.left + rc.right) / 2;
    const WORD y = (rc.top + rc.bottom) / 2;

    m_tooltips->SetTitle(info.szInfoTitle, info.uIcon);
    m_tooltips->TrackPosition(x, y);
    m_tooltips->UpdateTipText(m_hwndParent, reinterpret_cast<LPARAM>(m_toolbar->m_hWnd), info.szInfo);
    m_tooltips->TrackActivate(m_hwndParent, reinterpret_cast<LPARAM>(m_toolbar->m_hWnd));

    m_current = info.pSource;
    int timeout = info.uTimeout;
    if (timeout < MinTimeout) timeout = MinTimeout;
    if (timeout > MaxTimeout) timeout = MaxTimeout;

    SetTimer(timeout);
}

void CBalloonQueue::Close(IN OUT InternalIconData * notifyItem)
{
    TRACE("HideBalloonTip called\n");

    if (m_current == notifyItem && !m_currentClosed)
    {
        // Prevent Re-entry
        m_currentClosed = true;
        m_tooltips->TrackDeactivate();
        SetTimer(CooldownBetweenBalloons);
    }
}

/*
 * NotifyToolbar
 */

CNotifyToolbar::CNotifyToolbar() :
    m_ImageList(NULL),
    m_VisibleButtonCount(0),
    m_BalloonQueue(NULL)
{
}

CNotifyToolbar::~CNotifyToolbar()
{
}

int CNotifyToolbar::GetVisibleButtonCount()
{
    return m_VisibleButtonCount;
}

int CNotifyToolbar::FindItem(IN HWND hWnd, IN UINT uID, InternalIconData ** pdata)
{
    int count = GetButtonCount();

    for (int i = 0; i < count; i++)
    {
        InternalIconData * data = GetItemData(i);

        if (data->hWnd == hWnd &&
            data->uID == uID)
        {
            if (pdata)
                *pdata = data;
            return i;
        }
    }

    return -1;
}

int CNotifyToolbar::FindExistingSharedIcon(HICON handle)
{
    int count = GetButtonCount();
    for (int i = 0; i < count; i++)
    {
        InternalIconData * data = GetItemData(i);
        if (data->hIcon == handle)
        {
            TBBUTTON btn;
            GetButton(i, &btn);
            return btn.iBitmap;
        }
    }

    return -1;
}

BOOL CNotifyToolbar::AddButton(IN CONST NOTIFYICONDATA *iconData)
{
    TBBUTTON tbBtn;
    InternalIconData * notifyItem;
    WCHAR text[] = L"";

    TRACE("Adding icon %d from hWnd %08x flags%s%s state%s%s", 
        iconData->uID, iconData->hWnd,
        (iconData->uFlags & NIF_ICON) ? " ICON" : "",
        (iconData->uFlags & NIF_STATE) ? " STATE" : "",
        (iconData->dwState & NIS_HIDDEN) ? " HIDDEN" : "",
        (iconData->dwState & NIS_SHAREDICON) ? " SHARED" : "");

    int index = FindItem(iconData->hWnd, iconData->uID, &notifyItem);
    if (index >= 0)
    {
        TRACE("Icon %d from hWnd %08x ALREADY EXISTS!", iconData->uID, iconData->hWnd);
        return FALSE;
    }

    notifyItem = new InternalIconData();
    ZeroMemory(notifyItem, sizeof(*notifyItem));

    notifyItem->hWnd = iconData->hWnd;
    notifyItem->uID = iconData->uID;

    tbBtn.fsState = TBSTATE_ENABLED;
    tbBtn.fsStyle = BTNS_NOPREFIX;
    tbBtn.dwData = (DWORD_PTR)notifyItem;
    tbBtn.iString = (INT_PTR) text;
    tbBtn.idCommand = GetButtonCount();

    if (iconData->uFlags & NIF_STATE)
    {
        notifyItem->dwState = iconData->dwState & iconData->dwStateMask;
    }

    if (iconData->uFlags & NIF_MESSAGE)
    {
        notifyItem->uCallbackMessage = iconData->uCallbackMessage;
    }

    if (iconData->uFlags & NIF_ICON)
    {
        notifyItem->hIcon = iconData->hIcon;
        BOOL hasSharedIcon = notifyItem->dwState & NIS_SHAREDICON;
        if (hasSharedIcon)
        {
            INT iIcon = FindExistingSharedIcon(notifyItem->hIcon);
            if (iIcon < 0)
            {
                notifyItem->hIcon = NULL;
                TRACE("Shared icon requested, but HICON not found!!!");
            }
            tbBtn.iBitmap = iIcon;
        }
        else
        {
            tbBtn.iBitmap = ImageList_AddIcon(m_ImageList, notifyItem->hIcon);
        }
    }

    if (iconData->uFlags & NIF_TIP)
    {
        StringCchCopy(notifyItem->szTip, _countof(notifyItem->szTip), iconData->szTip);
    }

    if (iconData->uFlags & NIF_INFO)
    {
        // NOTE: In Vista+, the uTimeout value is disregarded, and the accessibility settings are used always.
        StrNCpy(notifyItem->szInfo, iconData->szInfo, _countof(notifyItem->szInfo));
        StrNCpy(notifyItem->szInfoTitle, iconData->szInfoTitle, _countof(notifyItem->szInfo));
        notifyItem->dwInfoFlags = iconData->dwInfoFlags;
        notifyItem->uTimeout = iconData->uTimeout;
    }

    if (notifyItem->dwState & NIS_HIDDEN)
    {
        tbBtn.fsState |= TBSTATE_HIDDEN;
    }
    else
    {
        m_VisibleButtonCount++;
    }

    /* TODO: support VERSION_4 (NIF_GUID, NIF_REALTIME, NIF_SHOWTIP) */

    CToolbar::AddButton(&tbBtn);
    SetButtonSize(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

    if (iconData->uFlags & NIF_INFO)
    {
        m_BalloonQueue->UpdateInfo(notifyItem);
    }

    return TRUE;
}

BOOL CNotifyToolbar::SwitchVersion(IN CONST NOTIFYICONDATA *iconData)
{
    InternalIconData * notifyItem;
    int index = FindItem(iconData->hWnd, iconData->uID, &notifyItem);
    if (index < 0)
    {
        WARN("Icon %d from hWnd %08x DOES NOT EXIST!", iconData->uID, iconData->hWnd);
        return FALSE;
    }

    if (iconData->uVersion != 0 && iconData->uVersion != NOTIFYICON_VERSION)
    {
        WARN("Tried to set the version of icon %d from hWnd %08x, to an unknown value %d. Vista+ program?", iconData->uID, iconData->hWnd, iconData->uVersion);
        return FALSE;
    }

    // We can not store the version in the uVersion field, because it's union'd with uTimeout,
    // which we also need to keep track of.
    notifyItem->uVersionCopy = iconData->uVersion;

    return TRUE;
}

BOOL CNotifyToolbar::UpdateButton(IN CONST NOTIFYICONDATA *iconData)
{
    InternalIconData * notifyItem;
    TBBUTTONINFO tbbi = { 0 };

    TRACE("Updating icon %d from hWnd %08x flags%s%s state%s%s",
        iconData->uID, iconData->hWnd,
        (iconData->uFlags & NIF_ICON) ? " ICON" : "",
        (iconData->uFlags & NIF_STATE) ? " STATE" : "",
        (iconData->dwState & NIS_HIDDEN) ? " HIDDEN" : "",
        (iconData->dwState & NIS_SHAREDICON) ? " SHARED" : "");

    int index = FindItem(iconData->hWnd, iconData->uID, &notifyItem);
    if (index < 0)
    {
        WARN("Icon %d from hWnd %08x DOES NOT EXIST!", iconData->uID, iconData->hWnd);
        return AddButton(iconData);
    }

    TBBUTTON btn;
    GetButton(index, &btn);
    int oldIconIndex = btn.iBitmap;

    tbbi.cbSize = sizeof(tbbi);
    tbbi.dwMask = TBIF_BYINDEX | TBIF_COMMAND;
    tbbi.idCommand = index;

    if (iconData->uFlags & NIF_STATE)
    {
        if (iconData->dwStateMask & NIS_HIDDEN &&
            (notifyItem->dwState & NIS_HIDDEN) != (iconData->dwState & NIS_HIDDEN))
        {
            tbbi.dwMask |= TBIF_STATE;
            if (iconData->dwState & NIS_HIDDEN)
            {
                tbbi.fsState |= TBSTATE_HIDDEN;
                m_VisibleButtonCount--;
            }
            else
            {
                tbbi.fsState &= ~TBSTATE_HIDDEN;
                m_VisibleButtonCount++;
            }
        }

        notifyItem->dwState &= ~iconData->dwStateMask;
        notifyItem->dwState |= (iconData->dwState & iconData->dwStateMask);
    }

    if (iconData->uFlags & NIF_MESSAGE)
    {
        notifyItem->uCallbackMessage = iconData->uCallbackMessage;
    }

    if (iconData->uFlags & NIF_ICON)
    {
        BOOL hasSharedIcon = notifyItem->dwState & NIS_SHAREDICON;
        if (hasSharedIcon)
        {
            INT iIcon = FindExistingSharedIcon(iconData->hIcon);
            if (iIcon >= 0)
            {
                notifyItem->hIcon = iconData->hIcon;
                tbbi.dwMask |= TBIF_IMAGE;
                tbbi.iImage = iIcon;
            }
            else
            {
                TRACE("Shared icon requested, but HICON not found!!! IGNORING!");
            }
        }
        else
        {
            notifyItem->hIcon = iconData->hIcon;
            tbbi.dwMask |= TBIF_IMAGE;
            tbbi.iImage = ImageList_ReplaceIcon(m_ImageList, oldIconIndex, notifyItem->hIcon);
        }
    }

    if (iconData->uFlags & NIF_TIP)
    {
        StringCchCopy(notifyItem->szTip, _countof(notifyItem->szTip), iconData->szTip);
    }

    if (iconData->uFlags & NIF_INFO)
    {
        // NOTE: In Vista+, the uTimeout value is disregarded, and the accessibility settings are used always.
        StrNCpy(notifyItem->szInfo, iconData->szInfo, _countof(notifyItem->szInfo));
        StrNCpy(notifyItem->szInfoTitle, iconData->szInfoTitle, _countof(notifyItem->szInfo));
        notifyItem->dwInfoFlags = iconData->dwInfoFlags;
        notifyItem->uTimeout = iconData->uTimeout;
    }

    /* TODO: support VERSION_4 (NIF_GUID, NIF_REALTIME, NIF_SHOWTIP) */

    SetButtonInfo(index, &tbbi);

    if (iconData->uFlags & NIF_INFO)
    {
        m_BalloonQueue->UpdateInfo(notifyItem);
    }

    return TRUE;
}

BOOL CNotifyToolbar::RemoveButton(IN CONST NOTIFYICONDATA *iconData)
{
    InternalIconData * notifyItem;

    TRACE("Removing icon %d from hWnd %08x", iconData->uID, iconData->hWnd);

    int index = FindItem(iconData->hWnd, iconData->uID, &notifyItem);
    if (index < 0)
    {
        TRACE("Icon %d from hWnd %08x ALREADY MISSING!", iconData->uID, iconData->hWnd);

        return FALSE;
    }

    if (!(notifyItem->dwState & NIS_HIDDEN))
    {
        m_VisibleButtonCount--;
    }

    if (!(notifyItem->dwState & NIS_SHAREDICON))
    {
        TBBUTTON btn;
        GetButton(index, &btn);
        int oldIconIndex = btn.iBitmap;
        ImageList_Remove(m_ImageList, oldIconIndex);

        // Update other icons!
        int count = GetButtonCount();
        for (int i = 0; i < count; i++)
        {
            TBBUTTON btn;
            GetButton(i, &btn);

            if (btn.iBitmap > oldIconIndex)
            {
                TBBUTTONINFO tbbi2 = { 0 };
                tbbi2.cbSize = sizeof(tbbi2);
                tbbi2.dwMask = TBIF_BYINDEX | TBIF_IMAGE;
                tbbi2.iImage = btn.iBitmap-1;
                SetButtonInfo(i, &tbbi2);
            }
        }
    }

    m_BalloonQueue->RemoveInfo(notifyItem);

    DeleteButton(index);

    delete notifyItem;

    return TRUE;
}

VOID CNotifyToolbar::ResizeImagelist()
{
    int cx, cy;
    HIMAGELIST iml;

    if (!ImageList_GetIconSize(m_ImageList, &cx, &cy))
        return;

    if (cx == GetSystemMetrics(SM_CXSMICON) && cy == GetSystemMetrics(SM_CYSMICON))
        return;

    iml = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ILC_COLOR32 | ILC_MASK, 0, 1000);
    if (!iml)
        return;

    ImageList_Destroy(m_ImageList);
    m_ImageList = iml;
    SetImageList(m_ImageList);

    int count = GetButtonCount();
    for (int i = 0; i < count; i++)
    {
        InternalIconData * data = GetItemData(i);
        BOOL hasSharedIcon = data->dwState & NIS_SHAREDICON;
        INT iIcon = hasSharedIcon ? FindExistingSharedIcon(data->hIcon) : -1;
        if (iIcon < 0)
            iIcon = ImageList_AddIcon(iml, data->hIcon);
        TBBUTTONINFO tbbi = { sizeof(tbbi), TBIF_BYINDEX | TBIF_IMAGE, 0, iIcon};
        SetButtonInfo(i, &tbbi);
    }

    SetButtonSize(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

VOID CNotifyToolbar::SendMouseEvent(IN WORD wIndex, IN UINT uMsg, IN WPARAM wParam)
{
    static LPCWSTR eventNames [] = {
        L"WM_MOUSEMOVE",
        L"WM_LBUTTONDOWN",
        L"WM_LBUTTONUP",
        L"WM_LBUTTONDBLCLK",
        L"WM_RBUTTONDOWN",
        L"WM_RBUTTONUP",
        L"WM_RBUTTONDBLCLK",
        L"WM_MBUTTONDOWN",
        L"WM_MBUTTONUP",
        L"WM_MBUTTONDBLCLK",
        L"WM_MOUSEWHEEL",
        L"WM_XBUTTONDOWN",
        L"WM_XBUTTONUP",
        L"WM_XBUTTONDBLCLK"
    };

    InternalIconData * notifyItem = GetItemData(wIndex);

    if (!::IsWindow(notifyItem->hWnd))
    {
        // We detect and destroy icons with invalid handles only on mouse move over systray, same as MS does.
        // Alternatively we could search for them periodically (would waste more resources).
        TRACE("Destroying icon %d with invalid handle hWnd=%08x\n", notifyItem->uID, notifyItem->hWnd);

        RemoveButton(notifyItem);

        HWND parentHWND = ::GetParent(::GetParent(GetParent()));
        ::SendMessage(parentHWND, WM_SIZE, 0, 0);

        return;
    }

    if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST)
    {
        TRACE("Sending message %S from button %d to %p (msg=%x, w=%x, l=%x)...\n",
                    eventNames[uMsg - WM_MOUSEFIRST], wIndex,
                    notifyItem->hWnd, notifyItem->uCallbackMessage, notifyItem->uID, uMsg);
    }

    DWORD pid;
    GetWindowThreadProcessId(notifyItem->hWnd, &pid);

    if (pid == GetCurrentProcessId() ||
        (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST))
    {
        ::PostMessage(notifyItem->hWnd,
                        notifyItem->uCallbackMessage,
                        notifyItem->uID,
                        uMsg);
    }
    else
    {
        SendMessage(notifyItem->hWnd,
                    notifyItem->uCallbackMessage,
                    notifyItem->uID,
                    uMsg);
    }
}

LRESULT CNotifyToolbar::OnMouseEvent(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

    INT iBtn = HitTest(&pt);

    if (iBtn >= 0)
    {
        SendMouseEvent(iBtn, uMsg, wParam);
    }

    bHandled = FALSE;
    return FALSE;
}

static VOID GetTooltipText(LPARAM data, LPTSTR szTip, DWORD cchTip)
{
    InternalIconData * notifyItem = reinterpret_cast<InternalIconData *>(data);
    if (notifyItem)
    {
        StringCchCopy(szTip, cchTip, notifyItem->szTip);
    }
    else
    {
        StringCchCopy(szTip, cchTip, L"");
    }
}

LRESULT CNotifyToolbar::OnTooltipShow(INT uCode, LPNMHDR hdr, BOOL& bHandled)
{
    RECT rcTip, rcItem;
    ::GetWindowRect(hdr->hwndFrom, &rcTip);

    SIZE szTip = { rcTip.right - rcTip.left, rcTip.bottom - rcTip.top };

    INT iBtn = GetHotItem();

    if (iBtn >= 0)
    {
        MONITORINFO monInfo = { 0 };
        HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);

        monInfo.cbSize = sizeof(monInfo);

        if (hMon)
            GetMonitorInfo(hMon, &monInfo);
        else
            ::GetWindowRect(GetDesktopWindow(), &monInfo.rcMonitor);

        GetItemRect(iBtn, &rcItem);

        POINT ptItem = { rcItem.left, rcItem.top };
        SIZE szItem = { rcItem.right - rcItem.left, rcItem.bottom - rcItem.top };
        ClientToScreen(&ptItem);

        ptItem.x += szItem.cx / 2;
        ptItem.y -= szTip.cy;

        if (ptItem.x + szTip.cx > monInfo.rcMonitor.right)
            ptItem.x = monInfo.rcMonitor.right - szTip.cx;

        if (ptItem.y + szTip.cy > monInfo.rcMonitor.bottom)
            ptItem.y = monInfo.rcMonitor.bottom - szTip.cy;

        if (ptItem.x < monInfo.rcMonitor.left)
            ptItem.x = monInfo.rcMonitor.left;

        if (ptItem.y < monInfo.rcMonitor.top)
            ptItem.y = monInfo.rcMonitor.top;

        TRACE("ptItem { %d, %d }\n", ptItem.x, ptItem.y);

        ::SetWindowPos(hdr->hwndFrom, NULL, ptItem.x, ptItem.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        return TRUE;
    }

    bHandled = FALSE;
    return 0;
}

void CNotifyToolbar::Initialize(HWND hWndParent, CBalloonQueue * queue)
{
    m_BalloonQueue = queue;

    DWORD styles =
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN |
        TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | TBSTYLE_WRAPABLE | TBSTYLE_TRANSPARENT |
        CCS_TOP | CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER;

    SubclassWindow(CToolbar::Create(hWndParent, styles));

    // Force the toolbar tooltips window to always show tooltips even if not foreground
    HWND tooltipsWnd = (HWND)SendMessageW(TB_GETTOOLTIPS);
    if (tooltipsWnd)
    {
        ::SetWindowLong(tooltipsWnd, GWL_STYLE, ::GetWindowLong(tooltipsWnd, GWL_STYLE) | TTS_ALWAYSTIP);
    }

    SetWindowTheme(m_hWnd, L"TrayNotify", NULL);

    m_ImageList = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ILC_COLOR32 | ILC_MASK, 0, 1000);        
    SetImageList(m_ImageList);

    TBMETRICS tbm = {sizeof(tbm)};
    tbm.dwMask = TBMF_BARPAD | TBMF_BUTTONSPACING | TBMF_PAD;
    tbm.cxPad = 1;
    tbm.cyPad = 1;
    tbm.cxButtonSpacing = 1;
    tbm.cyButtonSpacing = 1;
    SetMetrics(&tbm);

    SetButtonSize(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

/*
 * SysPagerWnd
 */
const WCHAR szSysPagerWndClass[] = L"SysPager";

CSysPagerWnd::CSysPagerWnd() {}
CSysPagerWnd::~CSysPagerWnd() {}

LRESULT CSysPagerWnd::DrawBackground(HDC hdc)
{
    RECT rect;

    GetClientRect(&rect);
    DrawThemeParentBackground(m_hWnd, hdc, &rect);

    return TRUE;
}

LRESULT CSysPagerWnd::OnEraseBackground(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    HDC hdc = (HDC) wParam;

    if (!IsAppThemed())
    {
        bHandled = FALSE;
        return 0;
    }

    return DrawBackground(hdc);
}

LRESULT CSysPagerWnd::OnCreate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    Toolbar.Initialize(m_hWnd, &m_BalloonQueue);
    CIconWatcher::Initialize(m_hWnd);

    HWND hWndTop = GetAncestor(m_hWnd, GA_ROOT);

    m_Balloons.Create(hWndTop, TTS_NOPREFIX | TTS_BALLOON | TTS_CLOSE);
        
    TOOLINFOW ti = { 0 };
    ti.cbSize = TTTOOLINFOW_V1_SIZE;
    ti.uFlags = TTF_TRACK | TTF_IDISHWND;
    ti.uId = reinterpret_cast<UINT_PTR>(Toolbar.m_hWnd);
    ti.hwnd = m_hWnd;
    ti.lpszText = NULL;
    ti.lParam = NULL;

    BOOL ret = m_Balloons.AddTool(&ti);
    if (!ret)
    {
        WARN("AddTool failed, LastError=%d (probably meaningless unless non-zero)\n", GetLastError());
    }

    m_BalloonQueue.Init(m_hWnd, &Toolbar, &m_Balloons);

    // Explicitly request running applications to re-register their systray icons
    ::SendNotifyMessageW(HWND_BROADCAST,
                            RegisterWindowMessageW(L"TaskbarCreated"),
                            0, 0);

    return TRUE;
}

LRESULT CSysPagerWnd::OnDestroy(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    m_BalloonQueue.Deinit();
    CIconWatcher::Uninitialize();
    return TRUE;
}

BOOL CSysPagerWnd::NotifyIconCmd(WPARAM wParam, LPARAM lParam)
{
    PCOPYDATASTRUCT cpData = (PCOPYDATASTRUCT) lParam;
    if (cpData->dwData == 1)
    {
        SYS_PAGER_COPY_DATA * data;
        NOTIFYICONDATA *iconData;
        BOOL ret = FALSE;

        int VisibleButtonCount = Toolbar.GetVisibleButtonCount();

        data = (PSYS_PAGER_COPY_DATA) cpData->lpData;
        iconData = &data->nicon_data;

        TRACE("NotifyIconCmd received. Code=%d\n", data->notify_code);
        switch (data->notify_code)
        {
        case NIM_ADD:
            ret = Toolbar.AddButton(iconData);
            if (ret == TRUE)
            {
                (void)AddIconToWatcher(iconData);
            }
            break;
        case NIM_MODIFY:
            ret = Toolbar.UpdateButton(iconData);
            break;
        case NIM_DELETE:
            ret = Toolbar.RemoveButton(iconData);
            if (ret == TRUE)
            {
                (void)RemoveIconFromWatcher(iconData);
            }
            break;
        case NIM_SETFOCUS:
            Toolbar.SetFocus();
            ret = TRUE;
        case NIM_SETVERSION:
            ret = Toolbar.SwitchVersion(iconData);
        default:
            TRACE("NotifyIconCmd received with unknown code %d.\n", data->notify_code);
            return FALSE;
        }

        if (VisibleButtonCount != Toolbar.GetVisibleButtonCount())
        {
            HWND parentHWND = ::GetParent(GetParent());
            ::SendMessage(parentHWND, WM_SIZE, 0, 0);
        }

        return ret;
    }

    return TRUE;
}

void CSysPagerWnd::GetSize(IN BOOL IsHorizontal, IN PSIZE size)
{
    /* Get the ideal height or width */
#if 0 
    /* Unfortunately this doens't work correctly in ros */
    Toolbar.GetIdealSize(!IsHorizontal, size);

    /* Make the reference dimension an exact multiple of the icon size */
    if (IsHorizontal)
        size->cy -= size->cy % GetSystemMetrics(SM_CYSMICON);
    else
        size->cx -= size->cx % GetSystemMetrics(SM_CXSMICON);

#else
    INT rows = 0;
    INT columns = 0;
    INT cyButton = GetSystemMetrics(SM_CYSMICON) + 2;
    INT cxButton = GetSystemMetrics(SM_CXSMICON) + 2;
    int VisibleButtonCount = Toolbar.GetVisibleButtonCount();

    if (IsHorizontal)
    {
        rows = max(size->cy / cyButton, 1);
        columns = (VisibleButtonCount + rows - 1) / rows;
    }
    else
    {
        columns = max(size->cx / cxButton, 1);
        rows = (VisibleButtonCount + columns - 1) / columns;
    }
    size->cx = columns * cxButton;
    size->cy = rows * cyButton;
#endif
}

LRESULT CSysPagerWnd::OnGetInfoTip(INT uCode, LPNMHDR hdr, BOOL& bHandled)
{
    NMTBGETINFOTIPW * nmtip = (NMTBGETINFOTIPW *) hdr;
    GetTooltipText(nmtip->lParam, nmtip->pszText, nmtip->cchTextMax);
    return TRUE;
}

LRESULT CSysPagerWnd::OnCustomDraw(INT uCode, LPNMHDR hdr, BOOL& bHandled)
{
    NMCUSTOMDRAW * cdraw = (NMCUSTOMDRAW *) hdr;
    switch (cdraw->dwDrawStage)
    {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT:
        return TBCDRF_NOBACKGROUND | TBCDRF_NOEDGES | TBCDRF_NOOFFSET | TBCDRF_NOMARK | TBCDRF_NOETCHEDEFFECT;
    }
    return TRUE;
}

LRESULT CSysPagerWnd::OnSize(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    LRESULT Ret = TRUE;
    SIZE szClient;
    szClient.cx = LOWORD(lParam);
    szClient.cy = HIWORD(lParam);

    Ret = DefWindowProc(uMsg, wParam, lParam);

    if (Toolbar)
    {
        Toolbar.SetWindowPos(NULL, 0, 0, szClient.cx, szClient.cy, SWP_NOZORDER);
        Toolbar.AutoSize();

        RECT rc;
        Toolbar.GetClientRect(&rc);

        SIZE szBar = { rc.right - rc.left, rc.bottom - rc.top };

        INT xOff = (szClient.cx - szBar.cx) / 2;
        INT yOff = (szClient.cy - szBar.cy) / 2;

        Toolbar.SetWindowPos(NULL, xOff, yOff, szBar.cx, szBar.cy, SWP_NOZORDER);
    }
    return Ret;
}

LRESULT CSysPagerWnd::OnCtxMenu(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    bHandled = TRUE;
    return 0;
}

LRESULT CSysPagerWnd::OnBalloonPop(UINT uCode, LPNMHDR hdr , BOOL& bHandled)
{
    m_BalloonQueue.CloseCurrent();
    bHandled = TRUE;
    return 0;
}

LRESULT CSysPagerWnd::OnTimer(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    if (m_BalloonQueue.OnTimer(wParam))
    {
        bHandled = TRUE;
    }

    return 0;
}

void CSysPagerWnd::ResizeImagelist()
{
    Toolbar.ResizeImagelist();
}

HWND CSysPagerWnd::_Init(IN HWND hWndParent, IN BOOL bVisible)
{
    DWORD dwStyle;

    /* Create the window. The tray window is going to move it to the correct
        position and resize it as needed. */
    dwStyle = WS_CHILD | WS_CLIPSIBLINGS;
    if (bVisible)
        dwStyle |= WS_VISIBLE;

    Create(hWndParent, 0, NULL, dwStyle);

    if (!m_hWnd)
    {
        return NULL;
    }

    SetWindowTheme(m_hWnd, L"TrayNotify", NULL);

    return m_hWnd;
}
