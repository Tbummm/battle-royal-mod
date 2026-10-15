//! Client-side drop presentation: the local plane model, the ride camera and the jump key.
//! Created by BR_GameMode on every machine with a screen (never on a dedicated server).
//! The plane is a local, non-networked model placed from BR_DropFlight each frame.
class BR_DropClient
{
	protected static const float CAMERA_DISTANCE = 55;
	protected static const float CAMERA_HEIGHT = 6;
	protected static const float CAMERA_LOOK_SPEED = 0.2;
	protected static const float CAMERA_MIN_PITCH = -10;
	protected static const float CAMERA_MAX_PITCH = 60;
	protected static const float CAMERA_FOV = 60;
	protected static const float CAMERA_FAR_PLANE = 8000;
	protected static const float PLANE_LINGER_SECONDS = 5;
	protected static const float JUMP_RETRY_SECONDS = 1;
	protected static const float JUMP_HINT_SECONDS = 15;

	protected BR_GameMode m_GameMode;
	protected IEntity m_Plane;
	protected CameraBase m_Camera;
	protected CameraBase m_PreviousCamera;
	protected float m_fCameraYaw;
	protected float m_fCameraPitch = 15;
	protected float m_fLastJumpRequest = -1000;
	protected int m_iHintShownForDrop;

	//------------------------------------------------------------------------------------------------
	void BR_DropClient(notnull BR_GameMode gameMode)
	{
		m_GameMode = gameMode;
		GetGame().GetCallqueue().CallLater(Update, 0, true);
	}

	//------------------------------------------------------------------------------------------------
	void ~BR_DropClient()
	{
		if (GetGame() && GetGame().GetCallqueue())
			GetGame().GetCallqueue().Remove(Update);

		ReleaseCamera();
		DeletePlane();
	}

	//------------------------------------------------------------------------------------------------
	//! Runs every frame.
	protected void Update()
	{
		BR_DropFlight flight = m_GameMode.GetDropFlight();
		float elapsed = m_GameMode.GetDropElapsed();
		if (!flight || elapsed > flight.GetDuration() + PLANE_LINGER_SECONDS)
		{
			ReleaseCamera();
			DeletePlane();
			return;
		}

		vector planeTransform[4];
		flight.GetTransform(elapsed, planeTransform);
		UpdatePlane(planeTransform);

		if (m_GameMode.IsLocalPlayerAboard())
		{
			UpdateCamera(planeTransform);
			HandleJumpInput(elapsed);
		}
		else
		{
			ReleaseCamera();
		}
	}

	//------------------------------------------------------------------------------------------------
	protected void UpdatePlane(vector transform[4])
	{
		if (!m_Plane)
		{
			ResourceName prefab = m_GameMode.GetDropPlanePrefab();
			if (prefab.IsEmpty())
				return;

			Resource resource = Resource.Load(prefab);
			if (!resource || !resource.IsValid())
				return;

			EntitySpawnParams params = new EntitySpawnParams();
			params.TransformMode = ETransformMode.WORLD;
			for (int i = 0; i < 4; i++)
			{
				params.Transform[i] = transform[i];
			}

			m_Plane = GetGame().SpawnEntityPrefabLocal(resource, GetGame().GetWorld(), params);
			return;
		}

		m_Plane.SetWorldTransform(transform);
	}

	//------------------------------------------------------------------------------------------------
	protected void DeletePlane()
	{
		if (m_Plane)
			SCR_EntityHelper.DeleteEntityAndChildren(m_Plane);

		m_Plane = null;
	}

	//------------------------------------------------------------------------------------------------
	//! Third-person orbit around the plane; mouse look turns it.
	protected void UpdateCamera(vector planeTransform[4])
	{
		if (!m_Camera && !CreateCamera())
			return;

		InputManager input = GetGame().GetInputManager();
		m_fCameraYaw += (input.GetActionValue("CharacterAimRight") - input.GetActionValue("CharacterAimLeft")) * CAMERA_LOOK_SPEED;
		m_fCameraPitch += (input.GetActionValue("CharacterAimDown") - input.GetActionValue("CharacterAimUp")) * CAMERA_LOOK_SPEED;
		m_fCameraPitch = Math.Clamp(m_fCameraPitch, CAMERA_MIN_PITCH, CAMERA_MAX_PITCH);

		vector rotation[3];
		float yaw = planeTransform[2].ToYaw() + m_fCameraYaw;
		Math3D.AnglesToMatrix(Vector(yaw, -m_fCameraPitch, 0), rotation);

		vector target = planeTransform[3] + vector.Up * CAMERA_HEIGHT;
		vector cameraTransform[4];
		cameraTransform[0] = rotation[0];
		cameraTransform[1] = rotation[1];
		cameraTransform[2] = rotation[2];
		cameraTransform[3] = target - rotation[2] * CAMERA_DISTANCE;
		m_Camera.SetWorldTransform(cameraTransform);
	}

	//------------------------------------------------------------------------------------------------
	protected bool CreateCamera()
	{
		EntitySpawnParams params = new EntitySpawnParams();
		params.TransformMode = ETransformMode.WORLD;
		m_Camera = CameraBase.Cast(GetGame().SpawnEntity(CameraBase, GetGame().GetWorld(), params));
		if (!m_Camera)
		{
			Print("[BR] Could not create the drop camera.", LogLevel.ERROR);
			return false;
		}

		m_Camera.SetVerticalFOV(CAMERA_FOV);
		m_Camera.SetFarPlane(CAMERA_FAR_PLANE);

		CameraManager cameraManager = GetGame().GetCameraManager();
		m_PreviousCamera = cameraManager.CurrentCamera();
		cameraManager.SetCamera(m_Camera);
		m_fCameraYaw = 0;
		return true;
	}

	//------------------------------------------------------------------------------------------------
	protected void ReleaseCamera()
	{
		if (!m_Camera)
			return;

		CameraManager cameraManager = GetGame().GetCameraManager();
		if (cameraManager && cameraManager.CurrentCamera() == m_Camera && m_PreviousCamera)
			cameraManager.SetCamera(m_PreviousCamera);

		SCR_EntityHelper.DeleteEntityAndChildren(m_Camera);
		m_Camera = null;
		m_PreviousCamera = null;
	}

	//------------------------------------------------------------------------------------------------
	protected void HandleJumpInput(float elapsed)
	{
		int dropId = m_GameMode.GetDropId();
		if (m_iHintShownForDrop != dropId)
		{
			m_iHintShownForDrop = dropId;
			SCR_HintManagerComponent.ShowCustomHint("Press Jump to leave the plane. You will be pushed out at the end of the route.", "Drop", JUMP_HINT_SECONDS);
		}

		if (!GetGame().GetInputManager().GetActionTriggered("CharacterJump"))
			return;

		// The server confirms by removing us from the aboard list; resend if that hasn't arrived yet
		if (elapsed - m_fLastJumpRequest < JUMP_RETRY_SECONDS)
			return;

		SCR_PlayerController playerController = SCR_PlayerController.Cast(GetGame().GetPlayerController());
		if (!playerController)
			return;

		m_fLastJumpRequest = elapsed;
		playerController.BR_RequestJump();
	}
}
