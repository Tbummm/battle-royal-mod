//! Phases a battle royale match moves through.
enum BR_EMatchPhase
{
	WARMUP,			//!< Waiting for enough players; respawns allowed
	COUNTDOWN,		//!< Enough players are in; the match starts when the timer runs out
	IN_PROGRESS,	//!< Match running (starts with the plane drop); no respawns
	ENDED			//!< Winner decided
}

[EntityEditorProps(category: "BattleRoyale/GameMode", description: "Battle royale game mode: warmup, team assignment, plane drop, elimination and win check.")]
class BR_GameModeClass : SCR_BaseGameModeClass
{
}

//! Battle royale match flow.
//! Server side: waits for players, assigns teams, flies them over the map in the drop plane,
//! tracks who is alive and ends the match when one team is left.
//! Match phase and drop state are replicated so clients can show the plane, camera and UI.
class BR_GameMode : SCR_BaseGameMode
{
	[Attribute("2", UIWidgets.Slider, "Players needed before the start countdown begins", "1 128 1", category: "Battle Royale")]
	protected int m_iMinPlayersToStart;

	[Attribute("30", UIWidgets.Slider, "Seconds between reaching the player minimum and the match starting", "5 300 1", category: "Battle Royale")]
	protected int m_iCountdownSeconds;

	[Attribute("1", UIWidgets.Slider, "Players per team: 1 solos, 2 duos, 3 trios, 4 squads", "1 4 1", category: "Battle Royale")]
	protected int m_iTeamSize;

	[Attribute("8", UIWidgets.Slider, "Seconds the winner message shows before the end screen", "0 60 1", category: "Battle Royale")]
	protected int m_iEndScreenDelaySeconds;

	[Attribute("1", UIWidgets.CheckBox, "At match start, delete everything players picked up during warmup (they keep their base uniform)", category: "Battle Royale")]
	protected bool m_bClearWarmupGear;

	[Attribute("0 0 0", UIWidgets.Coords, "Center of the playable area; the drop plane crosses near it. Leave at 0 0 0 to use the middle of the terrain", category: "Drop")]
	protected vector m_vMapCenter;

	[Attribute("800", UIWidgets.Slider, "How far (m) the plane's route may pass from the map center", "0 5000 50", category: "Drop")]
	protected float m_fFlightCenterJitter;

	[Attribute("6000", UIWidgets.Slider, "Length of the plane's route in metres", "1000 20000 100", category: "Drop")]
	protected float m_fFlightLength;

	[Attribute("800", UIWidgets.Slider, "Flight altitude above sea level in metres (Grey Man's free-fall needs 150 m above the ground)", "200 3000 10", category: "Drop")]
	protected float m_fFlightAltitude;

	[Attribute("70", UIWidgets.Slider, "Plane speed in metres per second", "20 200 1", category: "Drop")]
	protected float m_fFlightSpeed;

	[Attribute("200", UIWidgets.Slider, "Parachute opens automatically at this height above the ground (0 = never)", "0 1000 10", category: "Drop")]
	protected float m_fAutoDeployHeight;

	[Attribute("", UIWidgets.ResourceNamePicker, "Plane model flown over the map (BR_DropPlane.et)", "et", category: "Drop")]
	protected ResourceName m_DropPlanePrefab;

	[Attribute("", UIWidgets.ResourceNamePicker, "Parachute rig given to every player at match start (BR_ParachuteRig.et)", "et", category: "Drop")]
	protected ResourceName m_ParachuteRigPrefab;

	static const int TEAM_NONE = -1;
	protected static const int TICK_INTERVAL_MS = 1000;
	protected static const float HINT_DURATION = 6;
	protected static const float EXIT_BEHIND_DISTANCE = 20;		//!< Metres behind the plane's origin (the rear ramp)
	protected static const float EXIT_BELOW_DISTANCE = 6;
	protected static const float EXIT_SIDE_SPREAD = 8;				//!< Random sideways offset so jumpers don't stack
	protected static const float EXIT_FORWARD_SPEED_FACTOR = 0.3;	//!< Share of the plane's speed a jumper keeps
	protected static const int FREEFALL_ENGAGE_DELAY_MS = 1000;	//!< Let the teleport replicate before starting free-fall
	protected static const int AUTO_EJECT_INTERVAL_MS = 250;
	protected static const float DROP_BOUNDS_MARGIN = 150;		//!< Metres the route stays inside the terrain edge

	[RplProp(onRplName: "OnMatchPhaseReplicated")]
	protected BR_EMatchPhase m_eMatchPhase = BR_EMatchPhase.WARMUP;

	// Drop state; clients rebuild the flight path from these
	[RplProp(onRplName: "OnDropStateReplicated")]
	protected vector m_vDropStart;
	[RplProp(onRplName: "OnDropStateReplicated")]
	protected vector m_vDropEnd;
	[RplProp(onRplName: "OnDropStateReplicated")]
	protected float m_fDropSpeed;
	[RplProp(onRplName: "OnDropStateReplicated")]
	protected int m_iDropId;
	[RplProp(onRplName: "OnDropStateReplicated")]
	protected ref array<int> m_aAboardPlayers = {};

	// Server only
	protected int m_iCountdownRemaining;
	protected int m_iStartingTeams;
	protected bool m_bAutoEjectStarted;
	protected ref map<int, int> m_mPlayerTeams = new map<int, int>();
	protected ref array<int> m_aAlivePlayers = {};
	protected ref RandomGenerator m_Rng = new RandomGenerator();

	// Every machine
	protected ref BR_DropFlight m_DropFlight;
	protected int m_iDropFlightId;
	protected float m_fDropClockStartMs;
	protected ref BR_DropClient m_DropClient;

	//! Invoked with (BR_EMatchPhase newPhase) on the server and on clients.
	protected ref ScriptInvoker m_OnMatchPhaseChanged = new ScriptInvoker();

	//------------------------------------------------------------------------------------------------
	static BR_GameMode GetInstance()
	{
		if (!GetGame())
			return null;

		return BR_GameMode.Cast(GetGame().GetGameMode());
	}

	//------------------------------------------------------------------------------------------------
	BR_EMatchPhase GetMatchPhase()
	{
		return m_eMatchPhase;
	}

	//------------------------------------------------------------------------------------------------
	int GetTeamSize()
	{
		return m_iTeamSize;
	}

	//------------------------------------------------------------------------------------------------
	float GetAutoDeployHeight()
	{
		return m_fAutoDeployHeight;
	}

	//------------------------------------------------------------------------------------------------
	ResourceName GetDropPlanePrefab()
	{
		return m_DropPlanePrefab;
	}

	//------------------------------------------------------------------------------------------------
	ScriptInvoker GetOnMatchPhaseChanged()
	{
		return m_OnMatchPhaseChanged;
	}

	//------------------------------------------------------------------------------------------------
	//! Server only. Returns TEAM_NONE for players not in the current match.
	int GetPlayerTeam(int playerId)
	{
		int team;
		if (m_mPlayerTeams.Find(playerId, team))
			return team;

		return TEAM_NONE;
	}

	//------------------------------------------------------------------------------------------------
	//! Server only.
	bool IsPlayerInMatch(int playerId)
	{
		return m_aAlivePlayers.Contains(playerId);
	}

	//------------------------------------------------------------------------------------------------
	//! Current drop flight, or null when no drop has happened yet.
	BR_DropFlight GetDropFlight()
	{
		return m_DropFlight;
	}

	//------------------------------------------------------------------------------------------------
	int GetDropId()
	{
		return m_iDropId;
	}

	//------------------------------------------------------------------------------------------------
	//! Seconds since the plane started its route, on this machine's clock.
	float GetDropElapsed()
	{
		return (GetGame().GetWorld().GetWorldTime() - m_fDropClockStartMs) / 1000;
	}

	//------------------------------------------------------------------------------------------------
	bool IsPlayerAboard(int playerId)
	{
		return m_aAboardPlayers.Contains(playerId);
	}

	//------------------------------------------------------------------------------------------------
	bool IsLocalPlayerAboard()
	{
		PlayerController playerController = GetGame().GetPlayerController();
		return playerController && IsPlayerAboard(playerController.GetPlayerId());
	}

	//------------------------------------------------------------------------------------------------
	protected override void OnGameStart()
	{
		super.OnGameStart();

		if (!IsMaster())
			return;

		m_Rng.SetSeed(System.GetUnixTime());
		GetGame().GetCallqueue().CallLater(Tick, TICK_INTERVAL_MS, true);
		Print("[BR] Battle royale game mode started. Waiting for players.", LogLevel.NORMAL);

		if (m_DropPlanePrefab.IsEmpty() || m_ParachuteRigPrefab.IsEmpty())
			Print("[BR] Drop Plane Prefab or Parachute Rig Prefab is not set on the game mode; the drop will not work properly.", LogLevel.WARNING);
	}

	//------------------------------------------------------------------------------------------------
	void ~BR_GameMode()
	{
		if (GetGame() && GetGame().GetCallqueue())
			GetGame().GetCallqueue().Remove(Tick);
	}

	//------------------------------------------------------------------------------------------------
	//! Runs once a second on the server: drives warmup, countdown and the drop.
	protected void Tick()
	{
		if (m_eMatchPhase == BR_EMatchPhase.WARMUP)
		{
			if (CountLivingPlayers() >= m_iMinPlayersToStart)
				BeginCountdown();

			return;
		}

		if (m_eMatchPhase == BR_EMatchPhase.COUNTDOWN)
		{
			if (CountLivingPlayers() < m_iMinPlayersToStart)
			{
				SetMatchPhase(BR_EMatchPhase.WARMUP);
				BroadcastHint("Countdown cancelled", "Not enough players. Waiting for more to join.");
				return;
			}

			m_iCountdownRemaining--;
			if (m_iCountdownRemaining <= 0)
				StartMatch();
			else if (m_iCountdownRemaining <= 5 || m_iCountdownRemaining % 10 == 0)
				BroadcastHint("Match starting", string.Format("Match starts in %1 seconds.", m_iCountdownRemaining));

			return;
		}

		if (m_eMatchPhase == BR_EMatchPhase.IN_PROGRESS)
			TickDrop();
	}

	//------------------------------------------------------------------------------------------------
	protected void BeginCountdown()
	{
		m_iCountdownRemaining = m_iCountdownSeconds;
		SetMatchPhase(BR_EMatchPhase.COUNTDOWN);
		BroadcastHint("Match starting", string.Format("Enough players have joined. Match starts in %1 seconds.", m_iCountdownSeconds));
	}

	//------------------------------------------------------------------------------------------------
	protected void StartMatch()
	{
		array<int> players = {};
		GetLivingPlayers(players);

		AssignTeams(players);
		m_aAlivePlayers.Copy(players);

		array<int> teams = {};
		GetAliveTeams(teams);
		m_iStartingTeams = teams.Count();

		// Characters wait safely in the warmup area while their owners ride the plane.
		// The parachute rig is handed out at the moment of the jump, so it can't be dropped beforehand.
		foreach (int playerId : players)
		{
			IEntity character = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
			if (!character)
				continue;

			if (m_bClearWarmupGear)
				ClearWarmupGear(character);

			SetDamageEnabled(character, false);
		}

		BeginDrop(players);
		SetMatchPhase(BR_EMatchPhase.IN_PROGRESS);
		BroadcastHint("Match started", string.Format("%1 players in %2 teams. Last team standing wins.", players.Count(), m_iStartingTeams));
		PrintFormat("[BR] Match started: %1 players, %2 teams, team size %3.", players.Count(), m_iStartingTeams, m_iTeamSize);

		if (m_iStartingTeams <= 1)
			Print("[BR] Only one team in the match, so it will not end on its own (solo testing).", LogLevel.WARNING);
	}

	//------------------------------------------------------------------------------------------------
	//! Shuffles players and splits them into teams of m_iTeamSize.
	//! Team picking from the group menu comes later; for now teams are filled at random.
	protected void AssignTeams(notnull array<int> players)
	{
		m_mPlayerTeams.Clear();

		for (int i = players.Count() - 1; i > 0; i--)
		{
			int j = RandomIndex(i + 1);
			int swap = players[i];
			players[i] = players[j];
			players[j] = swap;
		}

		int teamSize = Math.Max(1, m_iTeamSize);
		foreach (int index, int playerId : players)
		{
			m_mPlayerTeams.Set(playerId, index / teamSize);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Deletes everything except the jacket, pants and boots, so warmup weapons and gear
	//! don't carry into the match and the backpack slot is free for the parachute rig.
	protected void ClearWarmupGear(notnull IEntity character)
	{
		SCR_InventoryStorageManagerComponent inventory = SCR_InventoryStorageManagerComponent.Cast(character.FindComponent(SCR_InventoryStorageManagerComponent));
		SCR_CharacterInventoryStorageComponent storage = SCR_CharacterInventoryStorageComponent.Cast(character.FindComponent(SCR_CharacterInventoryStorageComponent));
		if (!inventory || !storage)
			return;

		array<IEntity> keep = {};
		keep.Insert(storage.GetClothFromArea(LoadoutJacketArea));
		keep.Insert(storage.GetClothFromArea(LoadoutPantsArea));
		keep.Insert(storage.GetClothFromArea(LoadoutBootsArea));

		array<IEntity> items = {};
		inventory.GetItems(items);
		foreach (IEntity item : items)
		{
			// Items inside something already deleted are gone by the time we reach them
			if (item && !keep.Contains(item))
				inventory.TryDeleteItem(item);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Puts our parachute rig in the character's backpack slot.
	protected void GiveParachuteRig(notnull IEntity character)
	{
		if (m_ParachuteRigPrefab.IsEmpty())
			return;

		SCR_InventoryStorageManagerComponent inventory = SCR_InventoryStorageManagerComponent.Cast(character.FindComponent(SCR_InventoryStorageManagerComponent));
		if (!inventory || !inventory.TrySpawnPrefabToStorage(m_ParachuteRigPrefab, null, -1, EStoragePurpose.PURPOSE_LOADOUT_PROXY))
			Print("[BR] Could not equip a parachute rig; the backpack slot may be taken.", LogLevel.WARNING);
	}

	//------------------------------------------------------------------------------------------------
	protected void SetDamageEnabled(notnull IEntity character, bool enabled)
	{
		SCR_DamageManagerComponent damageManager = SCR_DamageManagerComponent.GetDamageManager(character);
		if (damageManager)
			damageManager.EnableDamageHandling(enabled);
	}

	//------------------------------------------------------------------------------------------------
	//! Picks a random route and puts every match player aboard.
	protected void BeginDrop(notnull array<int> players)
	{
		// Keep the route over the terrain: the game refuses to place players outside it
		vector boundsMin, boundsMax;
		GetGame().GetWorldEntity().GetTerrain(0, 0).GetTerrainBoundBox(boundsMin, boundsMax);
		boundsMin = boundsMin + Vector(DROP_BOUNDS_MARGIN, 0, DROP_BOUNDS_MARGIN);
		boundsMax = boundsMax - Vector(DROP_BOUNDS_MARGIN, 0, DROP_BOUNDS_MARGIN);

		vector center = m_vMapCenter;
		if (center == vector.Zero)
			center = (boundsMin + boundsMax) * 0.5;

		BR_DropFlight flight = BR_DropFlight.CreateRandom(m_Rng, center, m_fFlightCenterJitter, m_fFlightLength, m_fFlightAltitude, m_fFlightSpeed, boundsMin, boundsMax);
		m_vDropStart = flight.GetStart();
		m_vDropEnd = flight.GetEnd();
		m_fDropSpeed = flight.GetSpeed();
		m_aAboardPlayers.Copy(players);
		m_iDropId++;
		m_bAutoEjectStarted = false;
		Replication.BumpMe();
		OnDropStateReplicated();

		PrintFormat("[BR] Drop plane route %1 -> %2, %3 s.", m_vDropStart, m_vDropEnd, flight.GetDuration());
	}

	//------------------------------------------------------------------------------------------------
	//! Keeps client clocks in step and pushes out anyone still aboard at the end of the route.
	protected void TickDrop()
	{
		if (!m_DropFlight || m_aAboardPlayers.IsEmpty())
			return;

		float elapsed = GetDropElapsed();
		Rpc(RpcDo_SyncDropClock, m_iDropId, elapsed);

		if (m_bAutoEjectStarted || elapsed < m_DropFlight.GetDuration())
			return;

		m_bAutoEjectStarted = true;
		array<int> remaining = {};
		remaining.Copy(m_aAboardPlayers);
		foreach (int index, int playerId : remaining)
		{
			GetGame().GetCallqueue().CallLater(EjectPlayer, index * AUTO_EJECT_INTERVAL_MS, false, playerId);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Server only. Called when a player presses Jump in the plane.
	void HandleJumpRequest(int playerId)
	{
		if (!IsMaster() || m_eMatchPhase != BR_EMatchPhase.IN_PROGRESS)
			return;

		EjectPlayer(playerId);
	}

	//------------------------------------------------------------------------------------------------
	//! Moves the player's character behind the plane's ramp and starts Grey Man's free-fall.
	protected void EjectPlayer(int playerId)
	{
		if (!m_aAboardPlayers.Contains(playerId) || !m_DropFlight)
			return;

		m_aAboardPlayers.RemoveItem(playerId);
		Replication.BumpMe();
		OnDropStateReplicated();

		IEntity character = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
		if (!character)
			return;

		vector planeTransform[4];
		m_DropFlight.GetTransform(GetDropElapsed(), planeTransform);

		float sideways = (m_Rng.RandFloat01() * 2 - 1) * EXIT_SIDE_SPREAD;
		vector exitPosition = planeTransform[3] - planeTransform[2] * EXIT_BEHIND_DISTANCE + planeTransform[0] * sideways - vector.Up * EXIT_BELOW_DISTANCE;

		GiveParachuteRig(character);
		SetDamageEnabled(character, true);
		if (!SCR_Global.TeleportPlayer(playerId, exitPosition))
			Print(string.Format("[BR] Teleport to the plane failed for player %1 at %2 (outside the terrain?).", playerId, exitPosition), LogLevel.WARNING);

		vector exitVelocity = planeTransform[2] * m_DropFlight.GetSpeed() * EXIT_FORWARD_SPEED_FACTOR;
		GetGame().GetCallqueue().CallLater(EngageFreeFall, FREEFALL_ENGAGE_DELAY_MS, false, playerId, exitVelocity);
		PrintFormat("[BR] Player %1 jumped at %2.", playerId, exitPosition);
	}

	//------------------------------------------------------------------------------------------------
	//! Grey Man's usually engages on its own once the character is falling fast enough;
	//! this makes sure it does. StartFreeFall does nothing if free-fall is already running.
	protected void EngageFreeFall(int playerId, vector velocity)
	{
		IEntity character = GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId);
		if (!character)
			return;

		GMS_FreeFallComponent freeFall = GMS_FreeFallComponent.Cast(character.FindComponent(GMS_FreeFallComponent));
		if (freeFall)
			freeFall.StartFreeFall(velocity);
	}

	//------------------------------------------------------------------------------------------------
	//! Rebuilds the local flight and starts or stops the drop presentation.
	protected void OnDropStateReplicated()
	{
		if (m_iDropId == 0)
			return;

		if (m_iDropFlightId != m_iDropId)
		{
			m_iDropFlightId = m_iDropId;
			m_fDropClockStartMs = GetGame().GetWorld().GetWorldTime();
		}

		// Each property has its own callback, so rebuild whenever the route differs from what we hold
		if (!m_DropFlight || m_DropFlight.GetStart() != m_vDropStart || m_DropFlight.GetEnd() != m_vDropEnd)
			m_DropFlight = new BR_DropFlight(m_vDropStart, m_vDropEnd, m_fDropSpeed);

		if (!m_DropClient && RplSession.Mode() != RplMode.Dedicated)
			m_DropClient = new BR_DropClient(this);
	}

	//------------------------------------------------------------------------------------------------
	[RplRpc(RplChannel.Unreliable, RplRcver.Broadcast)]
	protected void RpcDo_SyncDropClock(int dropId, float elapsedSeconds)
	{
		if (dropId != m_iDropFlightId)
			return;

		m_fDropClockStartMs = GetGame().GetWorld().GetWorldTime() - elapsedSeconds * 1000;
	}

	//------------------------------------------------------------------------------------------------
	protected override void OnPlayerKilled(int playerId, IEntity playerEntity, IEntity killerEntity, notnull Instigator killer)
	{
		super.OnPlayerKilled(playerId, playerEntity, killerEntity, killer);

		if (IsMaster())
			EliminatePlayer(playerId);
	}

	//------------------------------------------------------------------------------------------------
	protected override void OnPlayerDisconnected(int playerId, KickCauseCode cause, int timeout)
	{
		if (IsMaster())
			EliminatePlayer(playerId);

		super.OnPlayerDisconnected(playerId, cause, timeout);
	}

	//------------------------------------------------------------------------------------------------
	protected void EliminatePlayer(int playerId)
	{
		if (m_eMatchPhase != BR_EMatchPhase.IN_PROGRESS || !m_aAlivePlayers.Contains(playerId))
			return;

		m_aAlivePlayers.RemoveItem(playerId);
		if (m_aAboardPlayers.Contains(playerId))
		{
			m_aAboardPlayers.RemoveItem(playerId);
			Replication.BumpMe();
		}

		array<int> teams = {};
		GetAliveTeams(teams);
		string playerName = GetGame().GetPlayerManager().GetPlayerName(playerId);
		BroadcastHint("Player eliminated", string.Format("%1 was eliminated. %2 teams remain.", playerName, teams.Count()));
		PrintFormat("[BR] %1 (id %2) eliminated. %3 players and %4 teams remain.", playerName, playerId, m_aAlivePlayers.Count(), teams.Count());

		CheckForWinner();
	}

	//------------------------------------------------------------------------------------------------
	protected void CheckForWinner()
	{
		if (m_eMatchPhase != BR_EMatchPhase.IN_PROGRESS || m_iStartingTeams <= 1)
			return;

		array<int> teams = {};
		GetAliveTeams(teams);
		if (teams.Count() > 1)
			return;

		// The whole winning team wins, including teammates who died along the way
		array<int> winners = {};
		if (teams.Count() == 1)
		{
			foreach (int playerId, int team : m_mPlayerTeams)
			{
				if (team == teams[0])
					winners.Insert(playerId);
			}
		}

		EndMatch(winners);
	}

	//------------------------------------------------------------------------------------------------
	protected void EndMatch(notnull array<int> winners)
	{
		SetMatchPhase(BR_EMatchPhase.ENDED);

		if (winners.IsEmpty())
		{
			BroadcastHint("Match over", "No team survived. It's a draw.");
			Print("[BR] Match ended in a draw.", LogLevel.NORMAL);
		}
		else
		{
			string names;
			foreach (int index, int playerId : winners)
			{
				if (index > 0)
					names += ", ";

				names += GetGame().GetPlayerManager().GetPlayerName(playerId);
			}

			BroadcastHint("Winner winner", string.Format("%1 won the match!", names));
			PrintFormat("[BR] Match won by: %1", names);
		}

		SCR_GameModeEndData endData = SCR_GameModeEndData.Create(EGameOverTypes.ENDREASON_UNDEFINED, winners);
		GetGame().GetCallqueue().CallLater(EndGameMode, m_iEndScreenDelaySeconds * 1000, false, endData);
	}

	//------------------------------------------------------------------------------------------------
	//! No respawning once the match is running; dead and late-joining players stay out.
	override bool CanPlayerSpawn_S(SCR_SpawnRequestComponent requestComponent, SCR_SpawnHandlerComponent handlerComponent, SCR_SpawnData data, out SCR_ESpawnResult result = SCR_ESpawnResult.SPAWN_NOT_ALLOWED)
	{
		if (m_eMatchPhase == BR_EMatchPhase.IN_PROGRESS || m_eMatchPhase == BR_EMatchPhase.ENDED)
		{
			result = SCR_ESpawnResult.SPAWN_NOT_ALLOWED;
			return false;
		}

		return super.CanPlayerSpawn_S(requestComponent, handlerComponent, data, result);
	}

	//------------------------------------------------------------------------------------------------
	protected void SetMatchPhase(BR_EMatchPhase phase)
	{
		if (m_eMatchPhase == phase)
			return;

		m_eMatchPhase = phase;
		Replication.BumpMe();
		OnMatchPhaseReplicated();
	}

	//------------------------------------------------------------------------------------------------
	protected void OnMatchPhaseReplicated()
	{
		m_OnMatchPhaseChanged.Invoke(m_eMatchPhase);
	}

	//------------------------------------------------------------------------------------------------
	protected int CountLivingPlayers()
	{
		array<int> players = {};
		GetLivingPlayers(players);
		return players.Count();
	}

	//------------------------------------------------------------------------------------------------
	protected void GetLivingPlayers(notnull array<int> outPlayers)
	{
		outPlayers.Clear();

		array<int> players = {};
		GetGame().GetPlayerManager().GetPlayers(players);
		foreach (int playerId : players)
		{
			if (IsPlayerCharacterAlive(playerId))
				outPlayers.Insert(playerId);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected bool IsPlayerCharacterAlive(int playerId)
	{
		ChimeraCharacter character = ChimeraCharacter.Cast(GetGame().GetPlayerManager().GetPlayerControlledEntity(playerId));
		if (!character)
			return false;

		CharacterControllerComponent controller = character.GetCharacterController();
		return controller && !controller.IsDead();
	}

	//------------------------------------------------------------------------------------------------
	protected void GetAliveTeams(notnull array<int> outTeams)
	{
		outTeams.Clear();
		foreach (int playerId : m_aAlivePlayers)
		{
			int team = GetPlayerTeam(playerId);
			if (!outTeams.Contains(team))
				outTeams.Insert(team);
		}
	}

	//------------------------------------------------------------------------------------------------
	//! Random index in [0, count).
	protected int RandomIndex(int count)
	{
		int index = m_Rng.RandFloat01() * count;
		return Math.ClampInt(index, 0, count - 1);
	}

	//------------------------------------------------------------------------------------------------
	//! Shows a hint to every player. Broadcast RPCs skip the sender, so the server shows it
	//! locally too, which covers hosted games and Workbench play mode.
	protected void BroadcastHint(string title, string message)
	{
		Rpc(RpcDo_ShowHint, title, message);
		RpcDo_ShowHint(title, message);
	}

	//------------------------------------------------------------------------------------------------
	[RplRpc(RplChannel.Reliable, RplRcver.Broadcast)]
	protected void RpcDo_ShowHint(string title, string message)
	{
		if (RplSession.Mode() == RplMode.Dedicated)
			return;

		SCR_HintManagerComponent.ShowCustomHint(message, title, HINT_DURATION);
	}
}
