//! Stops players dropping or moving their parachute rig while they are in the air.
//! Checked by the inventory UI on the client and validated again by the server.
modded class SCR_CharacterInventoryStorageComponent
{
	//------------------------------------------------------------------------------------------------
	//! Route removal checks through CanRemoveItem below (the engine caches this answer).
	override bool OnOverrideCanRemoveItem()
	{
		return true;
	}

	//------------------------------------------------------------------------------------------------
	override bool CanRemoveItem(IEntity item)
	{
		if (item && item.FindComponent(GMS_ParachuteBagComponent) && BR_IsOwnerInAir())
			return false;

		return super.CanRemoveItem(item);
	}

	//------------------------------------------------------------------------------------------------
	protected bool BR_IsOwnerInAir()
	{
		GMS_FreeFallComponent freeFall = GMS_FreeFallComponent.Cast(GetOwner().FindComponent(GMS_FreeFallComponent));
		return freeFall && freeFall.BR_IsInAir();
	}
}
