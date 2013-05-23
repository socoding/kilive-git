#include "GitMonitor.h"

HWND GitMonitor::m_window = NULL;

bool GitMonitor::PrepareHandles()
{
	DWORD attr = GetFileAttributes(m_config.m_monitor_path);
	if (attr == INVALID_FILE_ATTRIBUTES)
	{
		printf("File attribute error: %d\n, exit...", ::GetLastError());
		return false;
	}
	if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
	{
		printf("Must monitor a directory, exit...\n");
		return false;
	}

	m_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	if (INVALID_HANDLE_VALUE == m_iocp)
	{
		printf("Create IOCP error: %d, exit...\n", ::GetLastError());
		return false;
	}

	m_handle = ::CreateFile(m_config.m_monitor_path,
		FILE_LIST_DIRECTORY,
		FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,
		NULL);

	if(INVALID_HANDLE_VALUE == m_handle)
	{
		::CloseHandle(m_iocp);
		printf("Create root handle error: %d, exit...\n", ::GetLastError());
		return false;
	}

	if(NULL == ::CreateIoCompletionPort(m_handle, m_iocp, NULL, 0))
	{
		::CloseHandle(m_handle);
		::CloseHandle(m_iocp);
		printf("Bind to iocp error: %d, exit...\n", ::GetLastError());
		return false;
	}
	return true;
}

bool GitMonitor::Start(const char* config)
{
	if (!ResolveConfigPath(config)) return false;

	if (!PrepareHandles()) return false;

	if (!PostMonitor()) return false;

	ResolveConfigFile();
	
	Monitor();

	return true;
}

bool GitMonitor::PostMonitor()
{
	//We just monitor modification. Directory or file adding, rename or remove action with no modification will be ignored.
	::ZeroMemory(&m_overlapped, sizeof(m_overlapped));
	BOOL bSucceed = ::ReadDirectoryChangesW(m_handle,
		m_buffer,
		sizeof(m_buffer),
		TRUE,
		FILE_NOTIFY_CHANGE_LAST_WRITE, //| FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
		NULL,
		&m_overlapped,
		NULL);

	if(!bSucceed)
	{
		::CloseHandle(m_handle);
		::CloseHandle(m_iocp);
		printf("Post to IOCP error: %d, exit...\n", ::GetLastError());
		return false;
	}
	return true;
}

void GitMonitor::Monitor()
{
	DWORD wait_time = INFINITE;
	for (;;)
	{
		DWORD dwBytes;
		LPDWORD lpCompletionKey;
		OVERLAPPED* overlapped;
		wait_time = m_config.m_show_status >= 3 ? min((DWORD)(m_config.m_show_status), wait_time) : wait_time;
		BOOL bSucceed = ::GetQueuedCompletionStatus(m_iocp,
						&dwBytes,
						(LPDWORD)&lpCompletionKey,
						(LPOVERLAPPED*)&overlapped,
						wait_time);

		if (m_config.m_show_status >= 3)
		{
			m_config.m_show_status = m_config.m_show_status - wait_time;
			if (m_config.m_show_status <= 3)
			{
				m_config.m_show_status = 0;
				ShowWindow(m_window, SW_HIDE);
			}
		}

		clock_t current_tick = (clock_t)GetTickCount();
		if(bSucceed && overlapped)
		{
			assert(overlapped == &m_overlapped);

			FILE_NOTIFY_INFORMATION * pfiNotifyInfo = (FILE_NOTIFY_INFORMATION*)m_buffer;
			DWORD dwNextEntryOffset = 0;
			char sFileName[MAX_BUFFER_SIZE];
			do 
			{
				pfiNotifyInfo = (FILE_NOTIFY_INFORMATION*)((BYTE*)pfiNotifyInfo + dwNextEntryOffset);
				//We just monitor modification. Directory or file adding, rename or remove action with no modification will be ignored.
				if (pfiNotifyInfo->Action != FILE_ACTION_MODIFIED) continue;
				pfiNotifyInfo->FileName[pfiNotifyInfo->FileNameLength/sizeof(WCHAR)] = L'\0';
				WideCharToMultiByte(CP_ACP, NULL, pfiNotifyInfo->FileName, -1, sFileName, MAX_BUFFER_SIZE, NULL, FALSE);

				string src, dst;
				if (IsInTriggeredList(sFileName, src, dst))
					OnTriggered(current_tick, src, dst);

				dwNextEntryOffset = pfiNotifyInfo->NextEntryOffset;
			} while (dwNextEntryOffset != 0);
		}

		wait_time = UpdateTriggered(current_tick);

		if (!PostMonitor())
			break;
	}

	::CloseHandle(m_handle);
	::CloseHandle(m_iocp);
	
	for (TriggeredMap::iterator it = m_triggered_map.begin(); it != m_triggered_map.end(); ++it)
		delete it->second;
	m_triggered_map.clear();
	pqueue_clear(m_triggered_queue);

	ShowWindow(m_window, SW_SHOW);
}

int GitMonitor::OnTriggered(clock_t current_tick, const string& src, const string& dst)
{
	TriggeredMap::iterator src_it = m_triggered_map.find(src);
	if (src_it != m_triggered_map.end())
	{
		src_it->second->m_has_triggered = false;
		pqueue_change_priority(m_triggered_queue, src_it->second->m_triggered_index, (priority_t)(current_tick + m_config.m_delay_after_triggered));
		return 2;
	}

	TriggeredMap::iterator dst_it = m_triggered_map.find(dst);
	if (dst_it != m_triggered_map.end()) //copying from dst to src or in forbidden time ?
		return 0;

	Triggered* triggered = new Triggered();
	triggered->m_has_triggered = false;
	triggered->m_src_path = src;
	triggered->m_dst_path = dst;
	m_triggered_map.insert(TriggeredPair(src, triggered));
	element_t to_push = { (void*)triggered, (priority_t)(current_tick + m_config.m_delay_after_triggered), &(triggered->m_triggered_index) };
	pqueue_push(m_triggered_queue, to_push);
	return 1;
}

bool GitMonitor::ProcessTriggered(const Triggered* triggered)
{
	if (triggered->m_src_path.length() == 0) //config file triggered here!
	{
		ResolveConfigFile();
		return false;
	}

	DWORD attr = GetFileAttributes(triggered->m_src_path.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES)
	{
		printf("File %s attribute error: %d\n", triggered->m_src_path.c_str(), ::GetLastError());
		return false;
	}
	//CHAR src_buffer[_MAX_PATH];
	//WideCharToMultiByte(CP_ACP, NULL, triggered.m_src_path->c_str(), -1, src_buffer, _MAX_PATH, NULL, FALSE);
	//CHAR dst_buffer[_MAX_PATH];
	//WideCharToMultiByte(CP_ACP, NULL, triggered.m_dst_path->c_str(), -1, dst_buffer, _MAX_PATH, NULL, FALSE);
	//_wsystem(cmd_buffer);
	char cmd_buffer[_MAX_PATH * 2 + 32];
	if (attr & FILE_ATTRIBUTE_DIRECTORY)
	{
		sprintf(cmd_buffer, "ECHO D | XCOPY /E /H /Y /C /F \"%s\" \"%s\"", triggered->m_src_path.c_str(), triggered->m_dst_path.c_str());
	}
	else
	{
		sprintf(cmd_buffer, "ECHO F | XCOPY /H /Y /C /F \"%s\" \"%s\"", triggered->m_src_path.c_str(), triggered->m_dst_path.c_str());
	}
	system(cmd_buffer);
	return true;
}

DWORD GitMonitor::UpdateTriggered(clock_t current_tick)
{
	DWORD wait_time = INFINITE;
	while (!pqueue_is_empty(m_triggered_queue))
	{
		element_t top = pqueue_top(m_triggered_queue);
		if (!compare_priority((priority_t)current_tick, top.priority))
		{
			Triggered* triggered = (Triggered*)(top.user_data);
			if (!triggered->m_has_triggered && ProcessTriggered(triggered))
			{
				triggered->m_has_triggered = true;
				pqueue_change_priority(m_triggered_queue, triggered->m_triggered_index, (priority_t)(current_tick + m_config.m_delay_after_operation));
			}
			else
			{
				pqueue_pop(m_triggered_queue);
				m_triggered_map.erase(triggered->m_src_path);
				delete triggered;
			}
		}
		else
		{
			wait_time = (DWORD)(top.priority - (priority_t)current_tick);
			break;
		}
	}
	return wait_time;
}

bool GitMonitor::IsInTriggeredList(char* file_buffer, string& src, string& dst)
{
	if (strcmp(file_buffer, m_config.m_config_file) == 0)
		return true;

	for (int i = 0; i < m_config.m_replace_pair_count; ++i)
	{
		const ReplacePair* rep_pair = &m_config.m_replace_pairs[i];
		char* c_find_str = NULL;
		const char* c_src = NULL;
		const char* c_dst = NULL;
		if (c_find_str = strstr(file_buffer, rep_pair->m_ori_name))
		{
			c_src = rep_pair->m_ori_name;
			c_dst = rep_pair->m_rep_name;
		}
		else if (c_find_str = strstr(file_buffer, rep_pair->m_rep_name))
		{
			c_src = rep_pair->m_rep_name;
			c_dst = rep_pair->m_ori_name;
		}
		else
		{
			continue;
		}

		char check;
		if ((check = *(c_find_str + strlen(c_src))) != '\0' && check != DIR_SEP_C )  //is it full name?
			continue;
		if (c_find_str != file_buffer && *(c_find_str - 1) != DIR_SEP_C) //is it full name?
			continue;

		*c_find_str = '\0';
		src.assign(m_config.m_monitor_path);
		src.append(file_buffer);
		dst.assign(src);
		src.append(c_src);
		dst.append(c_dst);
		return true;
	}
	return false;
}

bool GitMonitor::ResolveConfigPath(const char* config)
{
	if (!config || *config == '\0')
	{
		printf("No config file input, exit...\n");
		return false;
	}

	if (!_fullpath(m_config.m_monitor_path, config, sizeof(m_config.m_monitor_path)))
	{
		printf("Config file path or name error, exit...\n");
		return false;
	}

	char* short_name = m_config.m_monitor_path + strlen(m_config.m_monitor_path);
	while (short_name > m_config.m_monitor_path && *(short_name - 1) != DIR_SEP_C)
		--short_name;

	if (strlen(short_name) >= sizeof(m_config.m_config_file))
	{
		printf("Config file name tooooo long, exit...\n");
		return false;
	}
	strcpy(m_config.m_config_file, short_name);
	*short_name = '\0';

	printf("Monitor path: \"%s\", config file: \"%s\"\n", m_config.m_monitor_path, m_config.m_config_file);

	ShowWindow(m_window, SW_SHOW);
	/* Init other config infos. */
	m_config.m_delay_after_triggered = MIN_DELAY_AFTER_TRIGGERED;
	m_config.m_delay_after_operation = MIN_DELAY_AFTER_OPERATION;
	m_config.m_delay_before_auto_hide = 0; //<=0 always show up >0 hide
	m_config.m_show_status = 2; //0:hidden; 1:need show up 2.shown up >=3.need hide time
	m_config.m_replace_pair_count = 0;

	return true;
}

bool GitMonitor::ResolveConfigFile()
{
	char config[_MAX_PATH];
	strcpy(config, m_config.m_monitor_path);
	strcat(config, m_config.m_config_file);

	lua_State* L = luaL_newstate();
	luaL_openlibs(L);  /* open libraries */
	int ret = luaL_dofile(L, config);
	if (ret == LUA_OK)
	{
		lua_checkstack(L, 64);
		clock_t delay;
		lua_getglobal(L, "delay_after_triggered");
		delay = lua_tointeger(L, -1);
		if (delay >= MIN_DELAY_AFTER_TRIGGERED)
			m_config.m_delay_after_triggered = delay;
		lua_getglobal(L, "delay_after_operation");
		delay = lua_tointeger(L, -1);
		if (delay >= MIN_DELAY_AFTER_OPERATION)
			m_config.m_delay_after_operation = delay;
		lua_getglobal(L, "delay_before_auto_hide");
		delay = lua_tointeger(L, -1);
		m_config.m_delay_before_auto_hide = delay <= 0 ? 0 : (delay < 3 ? 3 : delay);
		lua_getglobal(L, "replace_pair");
		if (!lua_istable(L, -1))
		{
			printf("`replace_pair` should be a 2-level array, so its value won't change here!\n");
		}
		else
		{
			int pair_count = 0;
			size_t len = lua_rawlen(L, -1), j;
			if (len > MAX_REPLACE_PAIR_COUNT)
			{
				len = MAX_REPLACE_PAIR_COUNT;
				printf("`replace_pair`size tooooo large. Max is %d, some may be omitted!\n", MAX_REPLACE_PAIR_COUNT);
			}
			for (j = 1; j <= len; j++)
			{
				lua_rawgeti(L, -1, j);
				if (lua_istable(L, -1))
				{
					lua_rawgeti(L, -1, 1);
					const char* name1 = lua_tostring(L, -1);
					if (!name1 || *name1 == '\0')
					{
						lua_pop(L, 2);
						continue;
					}
					lua_pop(L, 1);
					lua_rawgeti(L, -1, 2);
					const char* name2 = lua_tostring(L, -1);
					lua_pop(L, 2);
					if (!name2 || *name2 == '\0' || strcmp(name1, name2) == 0)
						continue;
					if (strlen(name1) >= MAX_SHORT_NAME || strlen(name2) >= MAX_SHORT_NAME)
					{
						printf("\"%s\" to \"%s\" in `replace_pair` may not take effect since name tooooo long! Max is %d.\n", name1, name2, MAX_SHORT_NAME);
						continue;
					}
					strcpy(m_config.m_replace_pairs[pair_count].m_ori_name, name1);		
					strcpy(m_config.m_replace_pairs[pair_count].m_rep_name, name2);
					++pair_count;
				}
				else
				{
					lua_pop(L, 1);
				}
			}
			m_config.m_replace_pair_count = pair_count;
		}
		//0:hidden; 1:need show up 2.shown up >=3.need hide time
		if (m_config.m_delay_before_auto_hide > 0) //hide after a while
		{
			if (m_config.m_show_status == 1 || m_config.m_show_status == 2)
				m_config.m_show_status = m_config.m_delay_before_auto_hide; //error resolved or shown up to hidden
		}
		else //always show
		{
			ShowWindow(m_window, SW_SHOW);
			m_config.m_show_status = 2;
		}
	}
	else
	{
		printf("Loading config file error. Nothing has been changed!\n");
		ShowWindow(m_window, SW_SHOW);
		m_config.m_show_status = 1; //until to be resolved.
	}

	printf("`delay_after_triggered` time is now %d ms!\n", m_config.m_delay_after_triggered);
	printf("`delay_after_operation` time is now %d ms!\n", m_config.m_delay_after_operation);
	printf("`delay_before_auto_hide` time is now %d ms!\n", m_config.m_delay_before_auto_hide);
	printf("Total `replace_pair_count` is %d\n", m_config.m_replace_pair_count);
	printf("=================================================================\n");

	lua_close(L);

	return ret == LUA_OK;
}

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam){
	DWORD pid;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId())
	{
		*((HWND*)lParam) = hwnd;
		return FALSE;
	}
	return TRUE;
}


void GitMonitor::InitWindowHandle()
{
	EnumWindows(EnumWindowsProc, (LPARAM)(&m_window));
}
