/*
OPCClientToolKit
Copyright (C) 2005 Mark C. Beharrell

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA  02111-1307, USA.
*/

#include "OPCServer.h"

#include <algorithm>

#ifdef OPCDA_CLIENT_NAMESPACE
namespace opcda_client
{
#endif

COPCServer::COPCServer(ATL::CComPtr<IOPCServer> &opcServerInterface)
{
    iOpcServer = opcServerInterface;

    HRESULT result = opcServerInterface->QueryInterface(IID_IOPCBrowseServerAddressSpace, (void **)&iOpcNameSpace);
    if (FAILED(result))
    {
        throw OPCException(L"COPCServer::COPCServer: FAILED to obtain IID_IOPCBrowseServerAddressSpace interface",
                           result);
    }

    result = opcServerInterface->QueryInterface(IID_IOPCItemProperties, (void **)&iOpcProperties);
    if (FAILED(result))
    {
        throw OPCException(L"COPCServer::COPCServer: FAILED to obtain IID_IOPCItemProperties interface", result);
    }

} // COPCServer::COPCServer

COPCServer::~COPCServer()
{
} // COPCServer::~COPCServer

COPCGroup *COPCServer::makeGroup(const std::wstring &groupName, bool active, unsigned long reqUpdateRate_ms,
                                 unsigned long &revisedUpdateRate_ms, float deadBand)
{
    return new COPCGroup(groupName, active, reqUpdateRate_ms, revisedUpdateRate_ms, deadBand, *this);

} // COPCServer::makeGroup

bool COPCServer::removeGroupByName(const std::wstring &name)
{
    ATL::CComPtr<IOPCGroupStateMgt> stateMgt;
    HRESULT result = iOpcServer->GetGroupByName(name.c_str(), IID_IOPCGroupStateMgt, (LPUNKNOWN *)&stateMgt);
    if (FAILED(result) || !stateMgt)
    {
        return false; // группа не найдена (или не видна из этого соединения)
    }

    DWORD updateRate = 0;
    BOOL active = FALSE;
    LPWSTR groupName = nullptr;
    LONG timeBias = 0;
    FLOAT deadband = 0;
    DWORD lcid = 0;
    OPCHANDLE clientHandle = 0;
    OPCHANDLE serverHandle = 0;

    result = stateMgt->GetState(&updateRate, &active, &groupName, &timeBias, &deadband, &lcid, &clientHandle,
                                &serverHandle);
    if (groupName != nullptr)
    {
        COPCClient::comFree(groupName);
    }

    if (FAILED(result))
    {
        return false;
    }

    result = iOpcServer->RemoveGroup(serverHandle, TRUE);
    return SUCCEEDED(result);

} // COPCServer::removeGroupByName

bool COPCServer::getItemNames(std::vector<std::wstring> &opcItemNames)
{
    if (!iOpcNameSpace)
    {
        return false;
    }

    // плоский браузинг (работает для плоских пространств имён
    // и для серверов, отдающих flat-представление от корня).
    if (browseFlat(opcItemNames) && !opcItemNames.empty())
    {
        return true;
    }

    // Иерархический браузинг для структурированных пространств имён в ветках
    opcItemNames.clear();
    if (!browseHierarchical(opcItemNames))
    {
        return false;
    }

    // Один и тот же тег сервер может отдавать и в flat-, и в иерархическом виде.
    std::sort(opcItemNames.begin(), opcItemNames.end());
    opcItemNames.erase(std::unique(opcItemNames.begin(), opcItemNames.end()), opcItemNames.end());

    return true;

} // COPCServer::getItemNames

bool COPCServer::browseFlat(std::vector<std::wstring> &opcItemNames)
{
    WCHAR emptyString[] = {0};

    ATL::CComPtr<IEnumString> iEnum;
    HRESULT result = iOpcNameSpace->BrowseOPCItemIDs(OPC_FLAT, emptyString, VT_EMPTY, 0, &iEnum);
    if (FAILED(result) || !iEnum)
    {
        return false;
    }

    for (;;)
    {
        LPWSTR name = nullptr;
        ULONG nameSize = 0;
        result = iEnum->Next(1, &name, &nameSize);
        if (result != S_OK || name == nullptr)
        {
            break;
        }

        LPWSTR fullName = nullptr;
        if (SUCCEEDED(iOpcNameSpace->GetItemID(name, &fullName)) && fullName != nullptr)
        {
            opcItemNames.push_back(fullName);
            COPCClient::comFree(fullName);
        }

        COPCClient::comFree(name);
    } 

    return true;

} // COPCServer::browseFlat

bool COPCServer::browseHierarchical(std::vector<std::wstring> &opcItemNames)
{
    // Возвращаемся к корню адресного пространства перед обходом.
    WCHAR emptyString[] = {0};
    if (FAILED(iOpcNameSpace->ChangeBrowsePosition(OPC_BROWSE_TO, emptyString)))
    {
        return false;
    }

    return browseHierarchicalNode(0, opcItemNames);

} // COPCServer::browseHierarchical

bool COPCServer::browseHierarchicalNode(int depth, std::vector<std::wstring> &opcItemNames)
{

    if (depth > 64)
    {
        return true;
    }

    // Листья в текущей позиции
    {
        ATL::CComPtr<IEnumString> iEnum;
        HRESULT result = iOpcNameSpace->BrowseOPCItemIDs(OPC_LEAF, L"", VT_EMPTY, 0, &iEnum);
        if (SUCCEEDED(result) && iEnum)
        {
            for (;;)
            {
                LPWSTR name = nullptr;
                ULONG nameSize = 0;
                result = iEnum->Next(1, &name, &nameSize);
                if (result != S_OK || name == nullptr)
                {
                    break;
                }

                LPWSTR fullName = nullptr;
                if (SUCCEEDED(iOpcNameSpace->GetItemID(name, &fullName)) && fullName != nullptr)
                {
                    opcItemNames.push_back(fullName);
                    COPCClient::comFree(fullName);
                }

                COPCClient::comFree(name);
            } 
        }     
    }        

    // Ветки в текущей позиции обходим
    {
        ATL::CComPtr<IEnumString> iEnum;
        HRESULT result = iOpcNameSpace->BrowseOPCItemIDs(OPC_BRANCH, L"", VT_EMPTY, 0, &iEnum);
        if (SUCCEEDED(result) && iEnum)
        {
            for (;;)
            {
                LPWSTR name = nullptr;
                ULONG nameSize = 0;
                result = iEnum->Next(1, &name, &nameSize);
                if (result != S_OK || name == nullptr)
                {
                    break;
                }

                std::wstring branchName(name);
                COPCClient::comFree(name);

                if (SUCCEEDED(iOpcNameSpace->ChangeBrowsePosition(OPC_BROWSE_DOWN, branchName.c_str())))
                {
                    browseHierarchicalNode(depth + 1, opcItemNames);
                    iOpcNameSpace->ChangeBrowsePosition(OPC_BROWSE_UP, L"");
                }
            }    
        }        
    }    

    return true;

} // COPCServer::browseHierarchicalNode

bool COPCServer::getStatus(ServerStatus &status)
{
    OPCSERVERSTATUS *serverStatus = nullptr;
    HRESULT result = iOpcServer->GetStatus(&serverStatus);
    if (FAILED(result))
    {
        throw OPCException(L"COPCServer::getStatus: FAILED to get status");
    }

    status.ftStartTime = serverStatus->ftStartTime;
    status.ftCurrentTime = serverStatus->ftCurrentTime;
    status.ftLastUpdateTime = serverStatus->ftLastUpdateTime;
    status.dwServerState = serverStatus->dwServerState;
    status.dwGroupCount = serverStatus->dwGroupCount;
    status.dwBandWidth = serverStatus->dwBandWidth;
    status.wMajorVersion = serverStatus->wMajorVersion;
    status.wMinorVersion = serverStatus->wMinorVersion;
    status.wBuildNumber = serverStatus->wBuildNumber;

    if (serverStatus->szVendorInfo)
    {
        status.vendorInfo = serverStatus->szVendorInfo;
        COPCClient::comFree(serverStatus->szVendorInfo);
    }

    COPCClient::comFree(serverStatus);
    return true;

} // COPCServer::getStatus

#ifdef OPCDA_CLIENT_NAMESPACE
} // namespace opcda_client
#endif
