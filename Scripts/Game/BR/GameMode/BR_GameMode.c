//! Phases a battle royale match moves through.
enum BR_EMatchPhase
{
	WARMUP,			//!< Waiting for enough players; respawns allowed
	COUNTDOWN,		//!< Enough players are in; the match starts when the timer runs out
	IN_PROGRESS,	//!< Match running; no respawns
	ENDED			//!< Winner decided
}

[EntityEditorProps(category: "BattleRoyale/GameMode", description: "Battle royale game mode: warmup, team assignment, elimination and win check.")]
class BR_GameModeClass : SCR_BaseGameModeClass
{
}

//! Battle royale match flow.
//! Server side: waits for players, assigns teams, places them, tracks who is alive and ends the match when one team is left.
//! The match phase is replicated so clients (UI, zone visuals) can react to it.
class BR_GameMode : SCR_BaseGameMode
{
	[Attribute("2", UIWidgets.Slider, "Players needed before the start countdown begins", "1 128 1", category: "Battle Royale")]
	protected int m_iMinPlayersToStart;

	[Attribute("30", UIWidgets.Slider, "Seconds between reaching the player minimum and the match starting", "5 300 1", category: "Battle Royale")]
	protected int m_iCountdownSeconds;

	[Attribute("1", UIWidgets.Slider, "Players per team: 1 solos, 2 duos, 3 trios, 4 squads", "1 4 1", category: "Battle Royale")]
	protected int m_iTeamSize;

	[Attribute("0 0 0", UIWidgets.Coords, "Center of the area teams are placed in at match start", category: "Battle Royale")]
	protected vector m_vStartAreaCenter;

	[Attribute("1500", UIWidgets.Slider, "Radius in metres of the start area", "100 10000 50", category: "Battle Royale")]
	protected float m_fStartAreaRadius;

	[Attribute("1", UIWidgets.EditBox, "Start positions with terrain lower than this height are rejected (keeps teams out of the sea)", category: "Battle Royale")]
	protected float m_fMinStartHeight;

	[Attribute("8", UIWidgets.Slider, "Seconds the winner message shows before the end screen", "0 60 1", category: "Battle Royale")]
	protected int m_iEndScreenDelaySeconds;

	static const int TEAM_NONE = -1;
	protected static const int TICK_INTERVAL_MS = 1000;
	protected static const float TEAMMATE_SPACING = 3;
	protected static const int START_POSITION_ATTEMPTS = 50;
	protected static const float HINT_DURATION = 6;

	[RplProp(onRplName: "OnMatchPhaseReplicated")]
	protected BR_EMatchPhase m_eMatchPhase = BR_EMatchPhase.WARMUP;

	// Server only
	protected int m_iCountdownRemaining;
	protected int m_iStartingTeams;
	protected ref map<int, int> m_mPlayerTeams = new map<int, int>();
	protected ref array<int> m_aAlivePlayers = {};
	protected ref RandomGenerator m_Rng = new RandomGenerator();

	//! Invoked with (BR_EMatchPhase newPhase) on the server and on clients.
	protected ref ScriptInvoker m_OnMatchPhaseChanged = new ScriptInvoker();

	//------------------------------------------------------------------------------------------------
	static BR_GameMode GetInstance()
	{
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
	protected override void OnGameStart()
	{
		super.OnGameStart();

		if (!IsMaster())
			return;

		m_Rng.SetSeed(System.GetUnixTime());
		GetGame().GetCallqueue().CallLater(Tick, TICK_INTERVAL_MS, true);
		Print("[BR] Battle royale game mode started. Waiting for players.", LogLevel.NORMAL);
	}

	//------------------------------------------------------------------------------------------------
	void ~BR_GameMode()
	{
		if (GetGame() && GetGame().GetCallqueue())
			GetGame().GetCallqueue().Remove(Tick);
	}

	//------------------------------------------------------------------------------------------------
	//! Runs once a second on the server and drives warmup and countdown.
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
		}
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
		PlaceTeams();

		array<int> teams = {};
		GetAliveTeams(teams);
		m_iStartingTeams = teams.Count();

		SetMatchPhase(BR_EMatchPhase.IN_PROGRESS);
		BroadcastHint("Match started", string.Format("%1 players in %2 teams. Last team standing wins.", players.Count(), m_iStartingTeams));
		PrintFormat("[BR] Match started: %1 players, %2 teams, team size %3.", players.Count(), m_iStartingTeams, m_iTeamSize);

		if (m_iStartingTeams <= 1)
			Print("[BR] Only one team in the match, so it will not end on its own (solo testing).", LogLevel.WARNING);
	}

	//------------------------------------------------------------------------------------------------
	//! Shuffles players and splits them into teams of m_iTeamSize.
	//! Parties and manual team choice come later; for now teams are filled at random.
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
	//! Puts each team at its own random spot in the start area, teammates side by side.
	//! Temporary until the parachute drop replaces it.
	protected void PlaceTeams()
	{
		BaseWorld world = GetGame().GetWorld();
		map<int, vector> teamOrigins = new map<int, vector>();
		map<int, int> teamMembersPlaced = new map<int, int>();

		foreach (int playerId, int team : m_mPlayerTeams)
		{
			vector origin;
			if (!teamOrigins.Find(team, origin))
			{
				origin = PickStartPosition();
				teamOrigins.Set(team, origin);
			}

			int memberIndex;
			teamMembersPlaced.Find(team, memberIndex);
			teamMembersPlaced.Set(team, memberIndex + 1);

			vector position = origin + Vector(memberIndex * TEAMMATE_SPACING, 0, 0);
			position[1] = world.GetSurfaceY(position[0], position[2]);
			SCR_Global.TeleportPlayer(playerId, position);
		}
	}

	//------------------------------------------------------------------------------------------------
	protected vector PickStartPosition()
	{
		BaseWorld world = GetGame().GetWorld();

		for (int attempt = 0; attempt < START_POSITION_ATTEMPTS; attempt++)
		{
			float angle = m_Rng.RandFloat01() * Math.PI2;
			float distance = Math.Sqrt(m_Rng.RandFloat01()) * m_fStartAreaRadius;
			vector position = m_vStartAreaCenter + Vector(Math.Cos(angle) * distance, 0, Math.Sin(angle) * distance);
			position[1] = world.GetSurfaceY(position[0], position[2]);

			if (position[1] >= m_fMinStartHeight)
				return position;
		}

		Print("[BR] No start position on land found; using the start area center. Check Start Area Center and Radius on the game mode.", LogLevel.WARNING);
		vector fallback = m_vStartAreaCenter;
		fallback[1] = world.GetSurfaceY(fallback[0], fallback[2]);
		return fallback;
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
