# OPCClientToolKit — справочник по API

Клиентская библиотека **OPC DA 2.05a** для C++ (x86/x64), собирается через CMake. Представляет объектную
обёртку над COM-интерфейсами OPC Data Access: хост → сервер → группа → элемент, плюс синхронный и
асинхронный ввод-вывод, подписка на изменения, refresh и чтение свойств элементов.

Документ составлен по исходникам каталога `OPCClientToolKit/`. Соответствие файлов и классов — в разделе 1.

Соглашения:

* `→ владеет вызывающий` — указатель необходимо освободить (`delete`, либо `deleteTransaction` для транзакций);
* методы, возвращающие `bool`, при неуспехе возвращают `false` **без** исключения; там, где в таблице указано
  «бросает», ошибка передаётся исключением `OPCException` (текст — `reasonString()`).

## Содержание

1. [Общая картина](#1-общая-картина)
2. [Жизненный цикл, COM и потоки](#2-жизненный-цикл-com-и-потоки)
3. [Справочник по методам](#3-справочник-по-методам)
4. [Сценарии использования](#4-сценарии-использования)
5. [Владение памятью](#5-владение-памятью)
6. [Известные ограничения и грабли](#6-известные-ограничения-и-грабли)
7. [Примеры в репозитории](#7-примеры-в-репозитории)
8. [Сборка и подключение](#8-сборка-и-подключение)

---

## 1. Общая картина

| Файл | Содержимое |
| --- | --- |
| `OPCClient.h` / `.cpp` | `COPCClient` (точка входа: инициализация COM, фабрика хостов, освобождение COM-памяти), `OPCException`, интерфейс `IAsyncDataCallback`, `enum OPCOLEInitMode` |
| `OPCHost.h` / `.cpp` | `COPCHost` (абстрактный), `CLocalHost`, `CRemoteHost` — хост-машина, перечисление OPC-серверов, подключение |
| `OPCServer.h` / `.cpp` | `COPCServer`, `struct ServerStatus` — обёртка `IOPCServer` + обзор namespace + свойства |
| `OPCGroup.h` / `.cpp` | `COPCGroup` и внутренний COM-объект `CAsyncDataCallback` (`IOPCDataCallback`) |
| `OPCItem.h` / `.cpp` | `COPCItem` — чтение/запись одного элемента и его свойства |
| `OPCItemData.h` / `.cpp` | `OPCItemData`, `COPCItemDataMap` — значение/качество/время/ошибка |
| `OPCProperties.h` / `.cpp` | `CPropertyDescription`, `SPropertyValue` — описатели свойств элементов |
| `Transaction.h` / `.cpp` | `CTransaction`, `ITransactionComplete` — завершение асинхронных операций |
| `OPCClientToolKitDLL.h` | макрос `OPCDACLIENT_API` (dllexport/dllimport; отключается `NO_OPCDACLIENT_LIBRARY`) |
| `opcda.h`, `opccomn.h`, `OpcEnum.h`, `*_i.c` | сгенерированные COM-заголовки OPC DA / OPC Common / OpcEnum (руками не править) |

Схема зависимостей:

```
COPCClient ──► COPCHost (CLocalHost | CRemoteHost) ──► COPCServer ──► COPCGroup ──► COPCItem
                                        CTransaction + ITransactionComplete   (async-результаты)
                                        IAsyncDataCallback                    (подписка на изменения)
                                        OPCItemData / COPCItemDataMap         (данные)
                                        OPCException                          (ошибки)
```

---

## 2. Жизненный цикл, COM и потоки

```cpp
COPCClient::init();                                   // 1 раз НА КАЖДЫЙ поток, работающий с OPC
COPCHost   *host = COPCClient::makeHost(L"localhost");
COPCServer *srv  = host->connectDAServer(L"Matrikon.OPC.Simulation.1");
unsigned long revisedRate = 0;
COPCGroup  *grp  = srv->makeGroup(L"Group", true, 1000, revisedRate, 0.0f);
COPCItem   *itm  = grp->addItem(name, true);
// ... работа ...
delete itm;  delete grp;  delete srv;  delete host;
COPCClient::stop();                                   // парно к init() в том же потоке
```

* `init()` / `stop()` используют счётчик `COPCClient::ReleaseCount`; вызывать строго попарно и в том же
  потоке, где работают с OPC-объектами. После `stop()` все объекты становятся недействительными.
* `init(APARTMENTTHREADED)` выполняет `CoInitialize(nullptr)` (STA; для MFC/GUI — обязательно в UI-потоке),
  `init(MULTITHREADED)` — `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` (объекты можно передавать между
  MTA-потоками).
* При первом `init()` вызывается `CoInitializeSecurity(RPC_C_AUTHN_LEVEL_NONE, RPC_C_IMP_LEVEL_IMPERSONATE,
  nullptr, EOAC_NONE, nullptr)` и кэшируется `IMalloc` (`CoGetMalloc(MEMCTX_TASK)`) — им пользуются
  `comFree()` / `comFreeVariant()`.
* Любая **асинхронная** операция (`readAsync`, `writeAsync`, `refresh`, подписка) требует: 1) предварительного
  `enableAsync()`; 2) живого COM-апартамента с прокачкой сообщений:

```cpp
#define MESSAGE_PUMP_UNTIL(x)                                \
    while (!x)                                               \
    {                                                        \
        MSG msg;                                             \
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))  \
        {                                                    \
            TranslateMessage(&msg);                          \
            DispatchMessage(&msg);                           \
        }                                                    \
        Sleep(10);                                           \
    }
```

(макрос из `OPCClientDemo.cpp`).

## 3. Справочник по методам

### 3.1 `COPCClient` — точка входа (все методы статические)

| Метод | Что делает | Возврат / ошибки |
| --- | --- | --- |
| `init(OPCOLEInitMode mode = APARTMENTTHREADED)` | Инициализирует COM текущего потока (`CoInitialize` / `CoInitializeEx`), настраивает `CoInitializeSecurity`, кэширует `IMalloc`; `++ReleaseCount` | `true`; бросает `OPCException`, если не удались `CoInitialize*` или `CoGetMalloc` |
| `stop()` | `--ReleaseCount`; при `<= 0` освобождает `IMalloc` и вызывает `CoUninitialize()` | `void` |
| `comFree(void *memory)` | Освобождает память, выделенную сервером/DCOM, через `IMalloc::Free`: строки `LPWSTR` от `GetItemID`, `OPCSERVERSTATUS`, массивы `HRESULT*`, `OPCITEMSTATE*`, `OPCITEMRESULT*` | `void` (`nullptr` не проверяется) |
| `comFreeVariant(VARIANT *memory, unsigned size)` | `VariantClear()` для каждого элемента + `Free()` массива | `void` |
| `makeHost(const std::wstring &hostName)` | Фабрика хоста: `""`, `"localhost"`, `"127.0.0.1"` → `CLocalHost`; иначе → `CRemoteHost` | `COPCHost*` → владеет вызывающий |
| `CATID_OPCDAv10` / `CATID_OPCDAv20` | Константы категорий для фильтра по версии сервера (`IID_CATID_OPCDAServer10/20`) | `const GUID` |
| `ReleaseCount` (public static) | Счётчик вложенных `init()` / `stop()` | `int` |

### 3.2 `COPCHost` / `CRemoteHost` / `CLocalHost`

Статические перекодировщики строк (объявлены в `COPCHost`): `WS2S`, `S2WS`, `WS2T`, `S2T`, `WS2LPCTSTR`, `S2LPCTSTR`, `LPCSTR2WS` (через `std::wstring_convert<codecvt_utf8<wchar_t>>`).

⚠ `WS2LPCTSTR()` / `S2LPCTSTR()` возвращают указатель на **временный** объект (`WS2T(...).c_str()`): использовать только немедленно в том же выражении, сохранять нельзя.

Общая (абстрактная) часть `COPCHost`:

| Метод | Что делает |
| --- | --- |
| `makeCOMObjectEx(hostName, tagCLSCTX serverLocation, const IID classID, const IID iid, void **interfacePtr)` (protected) | Создаёт COM-объект через `CoCreateInstanceEx` с `COAUTHINFO` (WINNT, `RPC_C_AUTHN_LEVEL_CONNECT`, `RPC_C_IMP_LEVEL_IMPERSONATE`); печатает `server name:` и `create instance error %x`; бросает `OPCException` при неуспехе |
| `getListOfDAServersEx(hostName, tagCLSCTX, CATID cid, std::vector<std::wstring> &progIDs, std::vector<CLSID> &classIDs)` (protected) | Создаёт `CLSID_OpcServerList` (OpcEnum), вызывает `IOPCServerList::EnumClassesOfCategories` по CATID, для каждого CLSID получает ProgID через `GetClassDetails`, печатает `prog ID: '...' - class ID: {...}`, заполняет оба вектора |
| `getListOfDAServers(CATID cid, std::vector<std::wstring> &listOfProgIDs, std::vector<CLSID> &listOfClassIDs)` (virtual, чистая) | Перечисление OPC DA-серверов на данном хосте |
| `getCLSID(const std::wstring &serverProgID)` (virtual, чистая) | CLSID по ProgID сервера |
| `connectDAServer(const std::wstring &serverProgID)` (virtual, чистая) | Подключение к серверу по ProgID |
| `connectDAServer(const CLSID &clsid)` (virtual, чистая) | Подключение к серверу по CLSID |

`CRemoteHost` (удалённый хост, `CLSCTX_REMOTE_SERVER`, через OpcEnum и удалённый реестр):

| Метод | Что делает |
| --- | --- |
| `CRemoteHost(const std::wstring &hostName)` | Запоминает имя хоста |
| `getListOfDAServers(...)` | `getListOfDAServersEx(HostName, CLSCTX_REMOTE_SERVER, ...)` |
| `getCLSID(progID)` | `IOPCServerList::CLSIDFromProgID` на удалённом хосте; бросает `OPCException` при неуспехе |
| `connectDAServer(const std::wstring &serverProgIDOrClsID)` | Если строка начинается с `'{'` — `CLSIDFromString`; иначе `GetCLSIDFromRemoteRegistry` (ветка `HKEY_LOCAL_MACHINE\SOFTWARE\Classes\<progID>\Guid` через `RegConnectRegistry` — **нужна служба RemoteRegistry**) → далее `connectDAServer(CLSID)` |
| `connectDAServer(const CLSID &serverClassID)` | `makeCOMObjectEx(..., CLSCTX_REMOTE_SERVER, ...)` + `QueryInterface(IID_IOPCServer)` → `new COPCServer` (владеет вызывающий) |
| `GetCLSIDFromRemoteRegistry(hostName, progID)` (private) | Вспомогательный разбор CLSID из удалённого реестра |

`CLocalHost` (локальный хост, `CLSCTX_LOCAL_SERVER`, через компонентные категории):

| Метод | Что делает |
| --- | --- |
| `getListOfDAServers(...)` | `getListOfDAServersEx(L"localhost", CLSCTX_LOCAL_SERVER, ...)` |
| `getCLSID(progID)` | ⚠ **не реализован**: `(void)progID; return CLSID();` — возвращает нулевой GUID, не использовать |
| `connectDAServer(const std::wstring &serverProgID)` | `CLSIDFromProgID` → `CoGetClassObject(CLSCTX_LOCAL_SERVER, IID_IClassFactory)` → `CreateInstance(IID_IUnknown)` → `QueryInterface(IID_IOPCServer)` → `new COPCServer`; каждый шаг бросает `OPCException` |
| `connectDAServer(const CLSID &clsid)` | То же, но CLSID задан напрямую (без `CLSIDFromProgID`) |

---

### 3.3 `COPCServer`

| Метод | Что делает | Возврат / ошибки |
| --- | --- | --- |
| `COPCServer(ATL::CComPtr<IOPCServer> &opcServerInterface)` | Сохраняет интерфейс сервера, запрашивает `IOPCBrowseServerAddressSpace` (namespace) и `IOPCItemProperties` | бросает `OPCException`, если сервер не отдал любой из интерфейсов |
| `~COPCServer()` | Пустой (COM-ссылки освобождаются `CComPtr`) | — |
| `getItemNames(std::vector<std::wstring> &opcItemNames)` | Обзор namespace в режиме **FLAT**: `QueryOrganization` (результат игнорируется), `BrowseOPCItemIDs(OPC_FLAT, ...)`, для каждого имени `GetItemID()`; имена **дописываются** в конец вектора (вектор не очищается), COM-строки освобождаются `comFree` | `false`, если нет browse-интерфейса или `BrowseOPCItemIDs` не удался; `true` **даже если ничего не найдено** |
| `makeGroup(const std::wstring &groupName, bool active, unsigned long reqUpdateRate_ms, unsigned long &revisedUpdateRate_ms, float deadBand)` | Создаёт OPC-группу в сервере (`IOPCServer::AddGroup`), возвращает `new COPCGroup` | `COPCGroup*` → владеет вызывающий |
| `getStatus(ServerStatus &status)` | `IOPCServer::GetStatus`; копирует поля структуры, `vendorInfo` копируется в `std::wstring` и освобождается | `true`; бросает `OPCException` при неуспехе (HRESULT при этом теряется) |
| `getServerInterface()` / `getPropertiesInterface()` (private, `friend` для `COPCGroup` / `COPCItem`) | Доступ к `IOPCServer` / `IOPCItemProperties` | `CComPtr<...>&` |

`struct ServerStatus`: `FILETIME ftStartTime`, `ftCurrentTime`, `ftLastUpdateTime`; `OPCSERVERSTATE dwServerState` (`OPC_STATUS_RUNNING`, `OPC_STATUS_FAILED`, `OPC_STATUS_NOCONFIG`, ...); `DWORD dwGroupCount`, `dwBandWidth`; `WORD wMajorVersion`, `wMinorVersion`, `wBuildNumber`; `std::wstring vendorInfo`.

### 3.4 `COPCGroup` — группы, чтение/запись, async, подписка

| Метод | Что делает | Возврат / ошибки |
| --- | --- | --- |
| `COPCGroup(groupName, bool active, unsigned long reqUpdateRate_ms, unsigned long &revisedUpdateRate_ms, float deadBand, COPCServer &server)` | `IOPCServer::AddGroup` + `QueryInterface` на `IOPCGroupStateMgt`, `IOPCSyncIO`, `IOPCAsyncIO2`, `IOPCItemMgt` | бросает `OPCException` с указанием, какой интерфейс не получен |
| `~COPCGroup()` | `IOPCServer::RemoveGroup(GroupHandle, false)` | — |
| `addItem(std::wstring &name, bool active)` | Добавляет один элемент (обёртка над `addItems`) | `COPCItem*` (владеет вызывающий); бросает, если элемент не создан |
| `addItems(std::vector<std::wstring> &names, std::vector<COPCItem*> &items, std::vector<HRESULT> &errors, bool active)` | Создаёт `COPCItem` на каждое имя, заполняет `OPCITEMDEF` (`hClient` = адрес `COPCItem`, `szItemID` = имя), вызывает `IOPCItemMgt::AddItems`; для успешных регистрирует запись в `GroupItemDataMap` и вызывает `setOPCParameters(hServer, vtCanonicalDataType, dwAccessRights)`; для неуспешных — `items[i] = nullptr`, `errors[i] = HRESULT` | **количество неудачных элементов**; бросает `OPCException`, если провалился общий вызов `AddItems` |
| `static OPCHANDLE getOpcHandle(void *ptr)` | «Клиентский handle» = значение указателя (`reinterpret_cast<uintptr_t>`); это ключ во всех картах данных | `OPCHANDLE` |
| `static OPCHANDLE addItemData(COPCItemDataMap &map, COPCItem *item, HRESULT error = S_OK)` | Создаёт в карте запись `OPCItemData(item, error)` по handle | `OPCHANDLE` |
| `lookupOpcItem(OPCHANDLE handle, COPCItem *&item)` | Обратный поиск элемента по handle в карте группы | `bool` (`item = nullptr`, если не найдено) |
| `getItemDataMap()` | Карта «все элементы группы → последние данные»; используется `refresh` | `COPCItemDataMap&` |
| `static DWORD getTransactionID(void *ptr)` | ID транзакции = значение указателя на `CTransaction` | `DWORD` |
| `addTransaction(CTransaction *transaction)` | Регистрирует активную async-транзакцию в `TransactionMap` | `DWORD` (ID транзакции) |
| `deleteTransaction(CTransaction *&transaction)` | Удаляет транзакцию из карты, `delete` объект, обнуляет указатель | `bool` (был ли ключ в карте) |
| `lookupTransaction(DWORD transactionID, CTransaction *&transaction)` | Поиск транзакции по ID | `bool` |
| `enableAsync(IAsyncDataCallback *handler)` | Включает асинхронный режим: `IConnectionPointContainer` → `FindConnectionPoint(IID_IOPCDataCallback)` → `Advise` (создаётся `CAsyncDataCallback`). **Обязателен для всех async-операций и подписки** | `true`; бросает, если async уже включён или connection point не найден |
| `disableAsync()` | `Unadvise`, сброс обработчика (объект освобождается COM-рефкаунтом) | `true`; бросает, если async не был включён |
| `setState(DWORD reqUpdateRate_ms, DWORD &returnedUpdateRate_ms, float deadBand, BOOL active)` | Изменяет состояние группы на лету (`IOPCGroupStateMgt::SetState`): период обновления, deadband, активность | `void`; бросает `OPCException` при неуспехе |
| `readSync(std::vector<COPCItem*> &items, COPCItemDataMap &opcData, OPCDATASOURCE source)` | Синхронное чтение группы (`IOPCSyncIO::Read`); `source` — `OPC_DS_CACHE` (из кэша сервера) или `OPC_DS_DEVICE` (с устройства); результаты (значение, качество, время, ошибка) складываются в `opcData` (ключ — handle, памятью владеет карта) | `void`; бросает `OPCException` при общем неуспехе; ошибки отдельных элементов — в `OPCItemData::Error` |
| `readAsync(std::vector<COPCItem*> &items, ITransactionComplete *transactionCB = nullptr)` | Асинхронное чтение группы (`IOPCAsyncIO2::Read`): создаёт транзакцию с записями на каждый элемент, регистрирует её и запускает чтение; если **все** элементы сразу вернули ошибку, транзакция помечается завершённой (колбэк не будет вызван) | `CTransaction*` → владеет вызывающий (освобождать `deleteTransaction`) |
| `refresh(OPCDATASOURCE source, ITransactionComplete *transactionCB = nullptr)` | `IOPCAsyncIO2::Refresh2`: сервер пришлёт значения **всех активных** элементов группы; данные попадут в карту транзакции (копию `GroupItemDataMap`) через `OnDataChange` | `CTransaction*` → владеет вызывающий |
| `cancelRefresh(CTransaction *&transaction)` | Снимает транзакцию (`deleteTransaction`) и вызывает `IOPCAsyncIO2::Cancel2(cancelID)`; `E_FAIL` игнорируется | `bool` (`false`, если транзакции нет) |
| `getSyncIOInterface()` / `getAsync2IOInterface()` / `getItemManagementInterface()` | Прямой доступ к COM-интерфейсам группы (используются `COPCItem`) | `CComPtr<...>&` |
| `getName()` | Имя группы | `const std::wstring&` |
| `getServer()` | Сервер-владелец группы | `COPCServer&` |
| `getUsrAsyncHandler()` | Текущий пользовательский обработчик асинхронных данных | `IAsyncDataCallback*` |

Внутренний COM-объект `CAsyncDataCallback : IOPCDataCallback` (объявлен в `OPCGroup.cpp`, снаружи недоступен):

| Метод | Что делает |
| --- | --- |
| `OnDataChange(transactionID, groupHandle, masterQuality, masterError, count, clientHandles, values, quality, time, errors)` | Если `transactionID != 0` — это ответ на `Refresh2`: данные пишутся в карту найденной транзакции и она помечается завершённой. Если `transactionID == 0` — изменение подписки: строится временная карта изменений и вызывается `IAsyncDataCallback::OnDataChange()` пользователя |
| `OnReadComplete(...)` | Данные async-чтения раскладываются по карте транзакции, затем `setCompleted()` |
| `OnWriteComplete(transactionID, groupHandle, masterError, count, clientHandles, errors)` | Записывает per-item ошибки в транзакцию, затем `setCompleted()` |
| `OnCancelComplete(transactionID, groupHandle)` | Только печатает сообщение в stdout |

---

### 3.5 `COPCItem`

| Метод | Что делает | Возврат / ошибки |
| --- | --- | --- |
| `COPCItem(std::wstring &itemName, COPCGroup &itemGroup)` (protected) | Элемент создаётся только через `COPCGroup::addItem()` / `addItems()` | — |
| `~COPCItem()` | `IOPCItemMgt::RemoveItems(1, &hServer, &err)`; результат не проверяется. **Удалять элементы до группы и сервера** | — |
| `setOPCParameters(OPCHANDLE handle, VARTYPE type, DWORD dwAccess)` | Заносит параметры, полученные от сервера при создании (`hServer`, канонический тип, права доступа) | `void` |
| `writeSync(VARIANT &data)` | Синхронная запись (`IOPCSyncIO::Write`) | `true`; бросает `OPCException` и при ошибке вызова, и при ошибке по элементу |
| `readSync(OPCItemData &data, OPCDATASOURCE source)` | Синхронное чтение элемента (делегируется в группу) с копированием результата в `data` | `true`; бросает, если элемент вернул ошибку или данных нет |
| `readAsync(ITransactionComplete *transactionCB = nullptr)` | Асинхронное чтение элемента (через группу) | `CTransaction*` → владеет вызывающий |
| `writeAsync(VARIANT &data, ITransactionComplete *transactionCB = nullptr)` | Асинхронная запись (`IOPCAsyncIO2::Write`); при немедленной ошибке транзакция сразу помечается завершённой | `CTransaction*` → владеет вызывающий |
| `getAccessRights()` | Права доступа элемента (биты `OPC_READABLE` / `OPC_WRITEABLE`) | `DWORD` |
| `getDataType()` | Канонический тип значения (`VT_I2`, `VT_R4`, `VT_BOOL`, `VT_BSTR`, ...); `VT_EMPTY`, если сервер не сообщил | `VARTYPE` |
| `getHandle()` | Серверный handle элемента (`hServer`) | `OPCHANDLE` |
| `getGroup()` | Группа-владелец | `COPCGroup&` |
| `getName()` | Имя элемента | `const std::wstring&` |
| `getSupportedProperties(std::vector<CPropertyDescription> &desc)` | `IOPCItemProperties::QueryAvailableProperties`: список доступных свойств элемента | `true`; бросает `OPCException` при неуспехе |
| `getProperties(const std::vector<CPropertyDescription> &propsToRead, ATL::CAutoPtrArray<SPropertyValue> &propsRead)` | `IOPCItemProperties::GetItemProperties`: значения запрошенных свойств. Элементы массива выровнены по запросу, но для свойств с ошибкой элемент **пуст (`nullptr`)** — проверяйте перед разыменованием | `true`; бросает `OPCException` при общем неуспехе |

### 3.6 `CTransaction` и `ITransactionComplete`

| Метод | Что делает |
| --- | --- |
| `CTransaction(ITransactionComplete *completeCB = nullptr)` | Пустая транзакция (для операций без данных) |
| `CTransaction(std::vector<COPCItem*> &items, ITransactionComplete *completeCB)` | Предсоздаёт по записи `OPCItemData(item, S_OK)` на каждый элемент — записи дозаполняются при завершении операции |
| `CTransaction(COPCItemDataMap &itemDataMap, ITransactionComplete *completeCB)` | **Глубокая копия** карты (используется `refresh`) |
| `getItemDataMap()` | Карта «handle → `OPCItemData`» (владеет значениями) |
| `setItemError(COPCItem *item, HRESULT error = S_OK)` | Внутренний: записывает ошибку по элементу (создаёт запись, если её не было); бросает, если элемент отсутствует в карте |
| `setItemValue(COPCItem *item, FILETIME time, WORD quality, VARIANT &value, HRESULT error = S_OK)` | Внутренний: записывает значение/качество/время/ошибку; бросает, если элемент отсутствует в карте |
| `getItemValue(COPCItem *item)` | Данные по элементу из карты транзакции; **не владеет**, может вернуть `nullptr`; бросает, если элемента нет в карте |
| `setCompleted()` | Помечает транзакцию завершённой и вызывает `ITransactionComplete::complete(*this)` |
| `isCompleted()` | Флаг завершения (используется в `MESSAGE_PUMP_UNTIL(transaction->isCompleted())`) |
| `setCancelId(DWORD)` / `getCancelId()` | CancelID, выданный сервером (для `Cancel2`) |

`ITransactionComplete::complete(CTransaction &transaction)` — реализуется клиентом; вызывается **в потоке COM-колбэка** (в STA — во время прокачки сообщений; надолго блокировать нельзя).

`IAsyncDataCallback::OnDataChange(COPCGroup &group, COPCItemDataMap &changes)` — уведомление об изменении активных элементов группы. Карта `changes` существует только на время вызова: читать внутри, сохранять указатели/данные нельзя.

### 3.7 Данные, карты, исключения

`OPCItemData`:

| Член | Смысл |
| --- | --- |
| `FILETIME ftTimeStamp` | метка времени значения от сервера |
| `WORD wQuality` | качество (`OPC_QUALITY_GOOD` / `..._BAD` / `..._NOTCONNECTED` и флаги) |
| `VARIANT vDataValue` | значение (`VariantClear` в деструкторе) |
| `HRESULT Error` | ошибка операции по этому элементу (`S_OK` при успехе) |
| `COPCItem *Item` | обратный указатель на элемент (не владеет) |
| `OPCItemData()`, `OPCItemData(COPCItem*, HRESULT = S_OK)`, `OPCItemData(COPCItem*, VARIANT&, WORD quality, FILETIME, HRESULT = S_OK)`, копирующий конструктор, `operator=` | `VariantInit` / `VariantCopy`; при неуспехе копирования бросают `OPCException` |
| `set(OPCITEMSTATE &itemState)` | Заполнение из COM-структуры (значение, качество, время) |
| `set(HRESULT error = S_OK)` | Только ошибка |
| `set(VARIANT &value, WORD quality, FILETIME time, HRESULT error = S_OK)` | Всё сразу |
| `item()` | `COPCItem*` (может быть `nullptr`) |

`COPCItemDataMap : CAtlMap<OPCHANDLE, OPCItemData*>` — **владеет** значениями (деструктор делает `delete`), `operator=` выполняет глубокую копию. Типовой обход (ключ читать **до** `GetNextValue`):

```cpp
POSITION pos = map.GetStartPosition();
while (pos)
{
    OPCHANDLE handle = map.GetKeyAt(pos);
    OPCItemData *data = map.GetNextValue(pos);
    if (data && data->item())
        printf("'%ws' handle=%u quality=%u value=%d\n",
               data->item()->getName().c_str(), handle, data->wQuality, data->vDataValue.iVal);
}
```

`OPCException : ATL::CAtlException` — конструктор `OPCException(const std::wstring &msg, HRESULT code = 0)`, но `code` **не сохраняется** (`(void)code;`); полезная информация — `reasonString()` (`const std::wstring&`). HRESULT внутрь исключения не попадает, ориентируйтесь на текст.

`CPropertyDescription { DWORD id; std::wstring desc; VARTYPE type; }`;
`SPropertyValue { const CPropertyDescription &propDesc; VARIANT value; }` (копирование значения через `VariantCopy`, освобождение через `VariantClear`).

---

## 4. Сценарии использования

Синхронное чтение/запись одного элемента:

```cpp
COPCClient::init();
COPCHost   *host = COPCClient::makeHost(COPCHost::S2WS("localhost"));
COPCServer *srv  = host->connectDAServer(L"Matrikon.OPC.Simulation.1");

unsigned long revisedRate = 0;
COPCGroup  *grp  = srv->makeGroup(L"Group", true, 1000, revisedRate, 0.0f);

std::wstring itemName = L"Bucket Brigade.UInt2";
COPCItem   *itm  = grp->addItem(itemName, true);

VARIANT v;  VariantInit(&v);
v.vt = VT_I2;  v.iVal = 998;
itm->writeSync(v);                                   // запись

OPCItemData data;                                    // чтение
itm->readSync(data, OPC_DS_DEVICE);                  // OPC_DS_DEVICE | OPC_DS_CACHE
printf("%ws = %d (quality %u)\n", itm->getName().c_str(), data.vDataValue.iVal, data.wQuality);
```

Асинхронное чтение с ожиданием завершения:

```cpp
class CTransComplete : public ITransactionComplete
{
  public:
    void complete(CTransaction &) { printf("async operation completed\n"); }
};

class CMyCallback : public IAsyncDataCallback
{
  public:
    void OnDataChange(COPCGroup &group, COPCItemDataMap &changes)
    {
        POSITION pos = changes.GetStartPosition();
        while (pos)
        {
            OPCHANDLE handle = changes.GetKeyAt(pos);
            OPCItemData *data = changes.GetNextValue(pos);
            if (data && data->item())
                printf("'%ws' handle=%u value=%d\n", data->item()->getName().c_str(), handle,
                       data->vDataValue.iVal);
        }
    }
};

CMyCallback usrCallBack;
grp->enableAsync(&usrCallBack);                      // обязательно до любых async-вызовов

CTransComplete complete;
CTransaction *t = grp->readAsync(items, &complete);  // или itm->readAsync(&complete)
MESSAGE_PUMP_UNTIL(t->isCompleted())                 // без прокачки сообщений колбэк не придёт

const OPCItemData *async = t->getItemValue(items[0]); // не владеет
if (async && !FAILED(async->Error))
    printf("value=%d quality=%u\n", async->vDataValue.iVal, async->wQuality);

grp->deleteTransaction(t);                           // удаляет из карты группы и delete
```

Подписка на изменения и refresh:

```cpp
grp->enableAsync(&usrCallBack);                            // изменения приходят в OnDataChange()
CTransaction *t = grp->refresh(OPC_DS_DEVICE, &complete);  // значения всех активных элементов
MESSAGE_PUMP_UNTIL(t->isCompleted())
grp->deleteTransaction(t);
// дальше достаточно продолжать прокачивать сообщения — изменения подписки придут сами
```

Пакетное чтение группы:

```cpp
COPCItemDataMap dataMap;                       // владеет найденными OPCItemData
grp->readSync(items, dataMap, OPC_DS_DEVICE);
// обход — см. раздел 3.7
```

---

## 5. Владение памятью

| Получено от | Как освобождать |
| --- | --- |
| `COPCClient::makeHost()` | `delete` (после удаления серверов) |
| `COPCHost::connectDAServer()` | `delete COPCServer*` (до `delete host`) |
| `COPCServer::makeGroup()` | `delete COPCGroup*` (до `delete server`) |
| `COPCGroup::addItem()` / `addItems()` | `delete COPCItem*` (до `delete group`) |
| `readAsync()` / `writeAsync()` / `refresh()` | `group->deleteTransaction(t)` — обычный `delete` недостаточен |
| COM-память (`LPWSTR`, массивы, структуры) | `COPCClient::comFree()` / `comFreeVariant()` |
| Данные в `COPCItemDataMap` / `CTransaction` | Владеет карта (или транзакция); вызывающему отдаются только невладеющие указатели |
| `getSupportedProperties()` / `getProperties()` | Владеет вызывающий (`CPropertyDescription` — `std::wstring`; `SPropertyValue` — в `CAutoPtrArray`) |

Почему именно `deleteTransaction`, а не `delete`: транзакция зарегистрирована в `TransactionMap` группы под
ключом-указателем. Если просто удалить объект, в карте останется «мёртвый» ключ, и следующий COM-колбэк
(например, поздний `OnReadComplete`) обратится к освобождённой памяти.

---

## 6. Известные ограничения и грабли

1. `COPCServer::getItemNames()` — только **FLAT**-режим (`OPC_FLAT`), результат `QueryOrganization`
   игнорируется; переданный вектор не очищается; при пустом namespace возвращается `true`, то есть «нет
   элементов» и «успех» неотличимы (`OPCServer.cpp:58-98`).
2. `CLocalHost::getCLSID()` — заглушка, возвращает нулевой CLSID (`OPCHost.cpp:310-315`).
3. `COPCGroup::addItems()` — освобождение `details[0].pBlob` вместо `details[i].pBlob` внутри цикла
   (`OPCGroup.cpp:481-484`): при нескольких элементах с BLOB — утечка/двойное освобождение.
4. «Клиентский handle» — это адрес `COPCItem` (`COPCGroup::getOpcHandle()`), что прямо помечено как
   рискованное решение в TODO (`OPCGroup.cpp:155`, `OPCGroup.cpp:173`): если сервер вернёт неизвестный
   handle, данные придут с `item() == nullptr`; если элемент уже удалён — обращение к освобождённой памяти.
5. Асинхронные операции требуют предварительного `enableAsync()` и работающего цикла сообщений
   (`MESSAGE_PUMP_UNTIL`), иначе колбэки не вызываются.
6. `OPCException` не сохраняет HRESULT (`code` игнорируется) — для диагностики доступен только текст.
7. `init()` / `stop()` — строго парно и по одному на поток; `COPCClient::comFree()` до первого `init()`
   недопустим (`iMalloc` пуст).
8. Библиотека и приложение должны использовать совместимые настройки CRT (Debug/Release не смешивать):
   через границу DLL передаются STL/ATL-типы (`std::vector<std::wstring>&`, `ATL::CAutoPtrArray`,
   `CAtlMap`).
9. `getProperties()`: элементы `propsRead` могут быть `nullptr` — проверять перед доступом.
10. `LocalSyncOPCCLient::SyncReadItem()` использует `static OPCItemData` — не потокобезопасно
    (`LocalSyncOPCCLient.cpp:190`).

---

## 7. Примеры в репозитории

| Проект | Что демонстрирует |
| --- | --- |
| `OPCClientDemo` | Полный сценарий: перечисление серверов (`CATID_OPCDAv20`), подключение, namespace, создание группы и элементов, sync/async чтение и запись, подписка (`CMyCallback`), `refresh`, `MESSAGE_PUMP_UNTIL` |
| `OPCPerformance` | Утилита: `OPCPerformance.exe <OPCItemList> <Host> <OPCServer> [noRefreshs]` — выгружает namespace в файл либо измеряет скорость `refresh` |
| `LocalSyncWrapper` (`LocalSyncOPCCLient`) | Обёртка локального сервера: `Init` / `Connect(serverName)` / `DisConnect` / `IsConnected` / `Stop`; `SyncReadItem` / `SyncWriteItem`; типизированные `ReadBool` / `WriteBool` (`VT_BOOL`), `ReadFloat` / `WriteFloat` (`VT_R4`), `ReadUint16` / `WriteUint16` (`VT_UI2`), `ReadUint16Array` / `WriteUint16Array` (`VT_ARRAY\|VT_UI2` через `SafeArray`); `IsOPCRuning()` (через `getStatus()` и `OPC_STATUS_RUNNING`); виртуальные точки расширения `IsOPCConnectedPLC()` и `ItemNameFilter()`; перед подключением проверяет службу `OpcEnum` (`DetectService`) |

`LocalSyncOPCCLient::Connect()` создаёт группу с периодом 1000 мс, `deadBand = 0.0` и добавляет все элементы
namespace, прошедшие `ItemNameFilter()`. Расширение под свои типы данных делается добавлением методов по
образцу существующих.

---

## 8. Сборка и подключение

* Корневой `CMakeLists.txt`: проект `opcclient` версии `0.5.2`.
  * `-DOPCDACLIENT_STATIC=ON` — статическая библиотека вместо DLL (автоматически добавляет
    `NO_OPCDACLIENT_LIBRARY` для примеров);
  * `-DOPCDACLIENT_TESTING=ON` — собирать `OPCClientDemo`, `OPCPerformance`, `LocalSyncWrapper`.
* Цель `OPCClientToolKit` (по умолчанию `SHARED`) устанавливается как `include/OPCClientToolKit` + `lib`/`bin`.
  В примерах: `include_directories(../OPCClientToolKit)` и `target_link_libraries(<app> OPCClientToolKit)`,
  поэтому DLL должна лежать рядом с exe (или в `PATH`).
* Экспорт символов: макрос `OPCDACLIENT_API` (`__declspec(dllexport)` при `OPCDACLIENT_LIBRARY`, иначе
  `dllimport`; при `NO_OPCDACLIENT_LIBRARY` — пусто).
* Необязательное пространство имён: определить `OPCDA_CLIENT_NAMESPACE`, чтобы все классы попали в
  `namespace opcda_client`.
* Требования: `OPC Core Components x64`; для перечисления серверов — служба `OpcEnum`; для удалённого
  подключения — настройки DCOM и служба `RemoteRegistry`.
* Для тестов подойдёт MatrikonOPC Simulation Server (см. корневой `README.md`).
