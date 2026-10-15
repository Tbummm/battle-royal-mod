//! Battle royale settings applied to Grey Man's free-fall component on every character.
modded class GMS_FreeFallComponent
{
	//------------------------------------------------------------------------------------------------
	override void OnPostInit(IEntity owner)
	{
		super.OnPostInit(owner);

		// Grey Man's ships with its developer canopy tuning mode switched on (Space under canopy enters it)
		m_bCanopyTuneMode = false;

		BR_GameMode gameMode = BR_GameMode.GetInstance();
		if (gameMode && gameMode.GetAutoDeployHeight() > 0)
			m_fAutoDeployHeight = gameMode.GetAutoDeployHeight();
	}
}
