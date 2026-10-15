//! Owner-to-server path for the drop jump. The player controller is the entity each client owns,
//! so it is where a client can send the server a request.
modded class SCR_PlayerController
{
	//------------------------------------------------------------------------------------------------
	void BR_RequestJump()
	{
		if (Replication.IsServer())
			RpcAsk_BRJump();
		else
			Rpc(RpcAsk_BRJump);
	}

	//------------------------------------------------------------------------------------------------
	[RplRpc(RplChannel.Reliable, RplRcver.Server)]
	protected void RpcAsk_BRJump()
	{
		BR_GameMode gameMode = BR_GameMode.GetInstance();
		if (gameMode)
			gameMode.HandleJumpRequest(GetPlayerId());
	}
}
