#ifndef GIT_SCANNER_H_
#define GIT_SCANNER_H_

#include <map>
#include <string>
#include <time.h>
#include <stdio.h>
#include <windows.h>
#include <winerror.h>
#include <sys/stat.h>
#include "assert.h"
#include "Pqueue.h"
#include "Lua.hpp"

#define MIN_DELAY_AFTER_TRIGGERED	100
#define MIN_DELAY_AFTER_OPERATION	100

#define MAX_SHORT_NAME				32
#define MAX_REPLACE_PAIR_COUNT		512
#define MAX_BUFFER_SIZE				1024

#define DIR_SEP_C		'\\'
#define DIR_SEP_S		"\\"

using namespace std;

typedef struct 
{
	char m_ori_name[MAX_SHORT_NAME];
	char m_rep_name[MAX_SHORT_NAME];
}ReplacePair;

typedef struct
{
	/* The first two member won't change again when start successfully. */
	char m_config_file[MAX_SHORT_NAME];
	char m_monitor_path[_MAX_PATH];
	clock_t m_delay_after_triggered;
	clock_t m_delay_after_operation;
	clock_t m_delay_before_auto_hide;
	int m_show_status;
	ReplacePair m_replace_pairs[MAX_REPLACE_PAIR_COUNT];
	int m_replace_pair_count;
}Config;

typedef struct
{
	pqueue_size_t m_triggered_index; //index in pqueue
	bool m_has_triggered;
	string m_src_path;
	string m_dst_path;
}Triggered;

typedef pqueue_t* TriggeredQueue;
typedef std::map<string, Triggered*> TriggeredMap;
typedef pair<string, Triggered*> TriggeredPair;

class GitMonitor
{
public:
	GitMonitor()
	{
		m_triggered_queue = pqueue_create(DEFAULT_PQUEUE_SIZE);
	}

	~GitMonitor()
	{
		pqueue_release(m_triggered_queue);
		m_triggered_queue = NULL;
	}

	bool Start(const char* config);

	static void InitWindowHandle();

private:
	bool ResolveConfigPath(const char* config);

	bool ResolveConfigFile();

	bool PrepareHandles();

	bool PostMonitor();

	void Monitor();

	int OnTriggered(clock_t current_tick, const string& src, const string& dst);

	bool ProcessTriggered(const Triggered* triggered);

	DWORD UpdateTriggered(clock_t current_tick);

	bool IsInTriggeredList(char* file_buffer, string& src, string& dst);

	Config m_config;
	
	OVERLAPPED m_overlapped;
	
	HANDLE m_iocp;
	
	HANDLE m_handle;

	BYTE m_buffer[MAX_BUFFER_SIZE];
	
	TriggeredMap m_triggered_map;

	TriggeredQueue m_triggered_queue;

	static HWND m_window;

	GitMonitor(const GitMonitor&);

	GitMonitor& operator = (const GitMonitor&);
};

#endif