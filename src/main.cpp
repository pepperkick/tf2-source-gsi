// Fix INVALID_HANDLE_VALUE redefinition warning
#ifndef _LINUX
#define WIN32_LEAN_AND_MEAN
#include "windows.h"
#endif

#ifndef NULL
#define NULL nullptr
#endif

#include "interface.h"
#include "filesystem.h"
#include "engine/iserverplugin.h"
#include "game/server/iplayerinfo.h"
#include "toolframework/ienginetool.h"
#include "eiface.h"
#include "igameevents.h"
#include "convar.h"
#include "Color.h"
#include "vstdlib/random.h"
#include "engine/IEngineTrace.h"
#include "tier2/tier2.h"
#include "ihltv.h"
#include "ihltvdirector.h"
#include "KeyValues.h"
#include "dt_send.h"
#include "server_class.h"
#include "cdll_int.h"
#include "shareddefs.h"

#include "common.h"
#include "player.h"
#include "ifaces.h"
#include "entities.h"

#include <vector>
#include <set>
#include <string>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

IServerGameDLL* g_pGameDLL;
IFileSystem* g_pFileSystem;
IHLTVDirector* g_pHLTVDirector;
IVEngineServer* engine;

bool poolReady = false;
bool breakPool = false;

int gAppId; 
const char* gVersion;

HWND gTimers;

void CALLBACK LoopTimer(HWND hwnd, UINT uMsg, UINT timerId, DWORD dwTime);

class Plugin : public IServerPluginCallbacks, IGameEventListener2 {
public:
    virtual bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory) override;
    virtual void Unload() override;
    virtual void Pause() override { }
    virtual void UnPause() override { }
    virtual const char* GetPluginDescription() override { return "GSI"; }
    virtual void LevelInit(const char* mapName) override { }
    virtual void ServerActivate(edict_t* pEdictList, int edictCount, int clientMax) override { }
    virtual void GameFrame(bool simulating) override { };
    virtual void LevelShutdown() override { }
    virtual void ClientActive(edict_t* pEntity) override { }
    virtual void ClientDisconnect(edict_t* pEntity) override { }
    virtual void ClientPutInServer(edict_t* pEntity, const char* playername) override;
    virtual void SetCommandClient(int index) override { }
    virtual void ClientSettingsChanged(edict_t* pEdict) override { }
    virtual PLUGIN_RESULT ClientConnect(bool* bAllowConnect, edict_t* pEntity, const char* pszName, const char* pszAddress, char* reject, int maxrejectlen) override { return PLUGIN_CONTINUE; }
    virtual PLUGIN_RESULT ClientCommand(edict_t* pEntity, const CCommand& args) override { return PLUGIN_CONTINUE; }
    virtual PLUGIN_RESULT NetworkIDValidated(const char* pszUserName, const char* pszNetworkID) override { return PLUGIN_CONTINUE; }
    virtual void OnQueryCvarValueFinished(QueryCvarCookie_t iCookie, edict_t* pPlayerEntity, EQueryCvarValueStatus eStatus, const char* pCvarName, const char* pCvarValue) override { }
    virtual void OnEdictAllocated(edict_t* edict) { }
    virtual void OnEdictFreed(const edict_t* edict) { }

    void FireGameEvent(IGameEvent* pEvent) override;

    void Transmit(const char* msg);
};

Plugin g_Plugin;
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(Plugin, IServerPluginCallbacks, INTERFACEVERSION_ISERVERPLUGINCALLBACKS, g_Plugin);

bool Plugin::Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory) {
    PRINT_TAG();
    ConColorMsg(Color(255, 255, 0, 255), "Loading plugin, Version: %s\n", PLUGIN_VERSION);

    Interfaces::Load(interfaceFactory, gameServerFactory);
    Interfaces::pGameEventManager->AddListener(this, "player_death", false);
    Interfaces::pGameEventManager->AddListener(this, "tf_game_over", false);
    Interfaces::pGameEventManager->AddListener(this, "teamplay_round_start", false);
    Interfaces::pGameEventManager->AddListener(this, "teamplay_round_active", false);
    Interfaces::pGameEventManager->AddListener(this, "teamplay_round_win", false);
    Interfaces::pGameEventManager->AddListener(this, "teamplay_round_stalemate", false);
    Interfaces::pGameEventManager->AddListener(this, "teamplay_game_over", false);

	gAppId = Interfaces::GetEngineClient()->GetAppID();
	gVersion = Interfaces::GetEngineClient()->GetProductVersionString();

	if (!Interfaces::GetClientDLL()) {
        PRINT_TAG();
        ConColorMsg(Color(255, 0, 0, 255), "Could not find game DLL interface, aborting load\n");
        return false;
    }

	if (!Interfaces::GetClientEngineTools()) {
        PRINT_TAG();
        ConColorMsg(Color(255, 0, 0, 255), "Could not find engine tools, aborting load\n");
        return false;
    }

	if (!Interfaces::GetEngineClient()) {
        PRINT_TAG();
        ConColorMsg(Color(255, 0, 0, 255), "Could not find engine client, aborting load\n");
        return false;
    }

	if (!Player::CheckDependencies()) {
		PRINT_TAG();
		ConColorMsg(Color(255, 0, 0, 255), "Required player helper class!\n");
		return false;
	}

	if (!Team::CheckDependencies()) {
		PRINT_TAG();
		ConColorMsg(Color(255, 0, 0, 255), "Required team helper class!\n");
		return false;
	}

    PRINT_TAG();
    ConColorMsg(Color(255, 255, 0, 255), "Successfully Started!\n");

    SetTimer(gTimers, 0, 25, &LoopTimer);

    return true;
}

void Plugin::Unload() {
    Interfaces::Unload();
    KillTimer(gTimers, 0);
}

void Plugin::ClientPutInServer(edict_t *pEntity, char const *playername) {
    PRINT_TAG();
    ConColorMsg(Color(255, 255, 255, 255), "Client Joined: %s\n", playername);
}

void Plugin::FireGameEvent(IGameEvent* pEvent) {
    PRINT_TAG();
    ConColorMsg(Color(255, 255, 255, 255), "Event: %s\n", pEvent->GetName());
}

void Plugin::Transmit(const char* msg) {
    HANDLE hPipe;
    LPTSTR lptPipeName = TEXT("\\\\.\\pipe\\tf2-gsi");

    hPipe = CreateFile(lptPipeName, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);

    DWORD cbWritten;
    WriteFile(hPipe, msg, strlen(msg), &cbWritten, NULL);
}

void CALLBACK LoopTimer(HWND hwnd, UINT uMsg, UINT timerId, DWORD dwTime) {
	Player localPlayer = Player::GetLocalPlayer();
	Player targetPlayer = Player::GetTargetPlayer();

	bool isInGame = Interfaces::GetEngineClient()->IsInGame();

	const char* mapName = Interfaces::GetEngineClient()->GetLevelName();

    std::ostringstream os;

    os << "{";
        os << "\"provider\": { "
                << "\"name\": \"Team Fortress 2\", "
                << "\"appid\": \"" << gAppId << "\", "
				<< "\"version\": \"" << gVersion << "\""
            << " }, ";

		if (targetPlayer) {
            os << "\"player\": { "
                << "\"name\": \"" << targetPlayer.GetName().c_str() << "\", "
                << "\"steamid\": \"" << targetPlayer.GetSteamID().ConvertToUint64() << "\" "
                << " }, ";
		}

		bool tvFlag = false, inGameFlag = false;

		if (isInGame) {
			if (!inGameFlag) {
				Player::FindPlayerResource();
				Team::FindTeams();
				RoundTimer::FindRoundTimer();

				inGameFlag = true;
			}

			os << "\"map\": { "
				<< "\"name\": \"" << mapName << "\", "
				<< " },";

			if (RoundTimer::GetRoundTimer()->IsValid()) {
				os << "\"round\": {"
					<< "\"isPuased\": \"" << RoundTimer::GetRoundTimer()->IsPaused() << "\", "
					<< "\"timeRemaining\": \"" << RoundTimer::GetRoundTimer()->GetTimeRemaining() << "\", "
					<< "\"maxLength\": \"" << RoundTimer::GetRoundTimer()->GetMaxLength() << "\", "
					<< "\"endTime\": \"" << RoundTimer::GetRoundTimer()->GetEndTime() - Interfaces::GetEngineTools()->ClientTime() << "\", "
					<< " }, ";
			}

			if (Team::GetRedTeam()->IsValid() && Team::GetBlueTeam()->IsValid()) {
				os << "\"teams\": {"
					<< "\"team_blue\": {"
					<< "\"name\": \"" << Team::GetBlueTeam()->GetName().c_str() << "\", "
					<< "\"score\": \"" << Team::GetBlueTeam()->GetScore() << "\", "
					<< " }, "
					<< "\"team_red\": {"
					<< "\"name\": \"" << Team::GetRedTeam()->GetName().c_str() << "\", "
					<< "\"score\": \"" << Team::GetRedTeam()->GetScore() << "\", "
					<< " }, "
					<< " }, ";
			}

			os << "\"allplayers\": { ";
			for (Player player : Player::Iterable()) {
				Vector position = player.GetPosition();

				if (!tvFlag) {
					tvFlag = true;
					continue;
				}

				os << "\"" << player.GetSteamID().ConvertToUint64() << "\": {"
					<< "\"name\": \"" << player.GetName().c_str()
					<< "\", \"team\": \"" << player.GetTeam()
					<< "\", \"health\": \"" << player.GetHealth()
					<< "\", \"class\": \"" << player.GetClass()
					<< "\", \"maxHealth\": \"" << player.GetMaxHealth()
					<< "\", \"weapon1\": \"" << player.GetWeapon(0000)
					<< "\", \"weapon2\": \"" << player.GetWeapon(0001)
					<< "\", \"weapon3\": \"" << player.GetWeapon(0002)
					<< "\", \"weapon4\": \"" << player.GetWeapon(0003)
					<< "\", \"alive\": \"" << player.IsAlive()
					<< "\", \"score\": \"" << player.GetTotalScore()
					<< "\", \"kills\": \"" << player.GetScore()
					<< "\", \"deaths\": \"" << player.GetDeaths()
					<< "\", \"damage\": \"" << player.GetDamage()
					<< "\", \"respawnTime\": \"" << player.GetRespawnTime() - Interfaces::GetEngineTools()->ClientTime()
					<< "\", \"position\": \"" << position.x << ", " << position.y << ", " << position.z << "\", ";

				if (player.GetClass() == TFClassType::TFClass_Medic) {
					int type = player.GetMedigunType();
					float charge = player.GetMedigunCharge();

					char name[64];
					sprintf(name, "Unknown");

					switch (type) {
					case TFMedigun_Unknown:
						sprintf(name, "Unknown");
						break;
					case TFMedigun_MediGun:
						sprintf(name, "MediGun");
						break;
					case TFMedigun_Kritzkrieg:
						sprintf(name, "Kritzkrieg");
						break;
					case TFMedigun_QuickFix:
						sprintf(name, "QuickFix");
						break;
					case TFMedigun_Vaccinator:
						sprintf(name, "Vaccinator");
						break;
					default:
						sprintf(name, "Unknown");
					}

					os << "\"medigun\": {"
						<< "\"type\": \"" << name << "\" , "
						<< "\"charge\": \"" << charge << "\""
						<< "}, ";
				}

				os << "}, ";
			}
			os << " }";
		}
		else {
			inGameFlag = false;
		}
    os << " }";

    g_Plugin.Transmit(os.str().c_str());
}