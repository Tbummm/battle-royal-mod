//! Straight-line flight path of the drop plane.
//! Pure math: every machine builds the same path from a few replicated numbers and places
//! the plane itself, so the plane's position never has to be streamed.
class BR_DropFlight
{
	protected vector m_vStart;
	protected vector m_vEnd;
	protected float m_fSpeed;
	protected vector m_vDirection;
	protected float m_fDuration;

	//------------------------------------------------------------------------------------------------
	void BR_DropFlight(vector start, vector end, float speed)
	{
		m_vStart = start;
		m_vEnd = end;
		m_fSpeed = Math.Max(1, speed);
		m_vDirection = (end - start).Normalized();
		m_fDuration = vector.Distance(start, end) / m_fSpeed;
	}

	//------------------------------------------------------------------------------------------------
	//! Random path at a fixed altitude, crossing near the given center from a random heading.
	//! The path is cut short where it would leave the area between boundsMin and boundsMax
	//! (players can't be placed outside the terrain).
	static BR_DropFlight CreateRandom(notnull RandomGenerator rng, vector center, float centerJitter, float length, float altitude, float speed, vector boundsMin, vector boundsMax)
	{
		float jitterAngle = rng.RandFloat01() * Math.PI2;
		float jitterDistance = Math.Sqrt(rng.RandFloat01()) * centerJitter;
		vector crossing = center + Vector(Math.Cos(jitterAngle) * jitterDistance, 0, Math.Sin(jitterAngle) * jitterDistance);
		crossing[0] = Math.Clamp(crossing[0], boundsMin[0], boundsMax[0]);
		crossing[2] = Math.Clamp(crossing[2], boundsMin[2], boundsMax[2]);
		crossing[1] = altitude;

		float heading = rng.RandFloat01() * Math.PI2;
		vector direction = Vector(Math.Cos(heading), 0, Math.Sin(heading));
		float halfLength = length * 0.5;
		float forward = Math.Min(halfLength, DistanceToBounds(crossing, direction, boundsMin, boundsMax));
		float backward = Math.Min(halfLength, DistanceToBounds(crossing, -direction, boundsMin, boundsMax));

		return new BR_DropFlight(crossing - direction * backward, crossing + direction * forward, speed);
	}

	//------------------------------------------------------------------------------------------------
	//! How far from origin along a horizontal direction before leaving the box (X and Z only).
	protected static float DistanceToBounds(vector origin, vector direction, vector boundsMin, vector boundsMax)
	{
		float distance = float.MAX;
		for (int axis = 0; axis < 3; axis += 2)
		{
			if (direction[axis] > 0.0001)
				distance = Math.Min(distance, (boundsMax[axis] - origin[axis]) / direction[axis]);
			else if (direction[axis] < -0.0001)
				distance = Math.Min(distance, (boundsMin[axis] - origin[axis]) / direction[axis]);
		}

		return Math.Max(0, distance);
	}

	//------------------------------------------------------------------------------------------------
	vector GetStart()
	{
		return m_vStart;
	}

	//------------------------------------------------------------------------------------------------
	vector GetEnd()
	{
		return m_vEnd;
	}

	//------------------------------------------------------------------------------------------------
	float GetSpeed()
	{
		return m_fSpeed;
	}

	//------------------------------------------------------------------------------------------------
	//! Seconds from the start of the path to the end.
	float GetDuration()
	{
		return m_fDuration;
	}

	//------------------------------------------------------------------------------------------------
	vector GetDirection()
	{
		return m_vDirection;
	}

	//------------------------------------------------------------------------------------------------
	vector GetPosition(float elapsedSeconds)
	{
		float t = Math.Clamp(elapsedSeconds / m_fDuration, 0, 1);
		return m_vStart + (m_vEnd - m_vStart) * t;
	}

	//------------------------------------------------------------------------------------------------
	//! Plane transform: nose (+Z) along the flight direction, level flight.
	void GetTransform(float elapsedSeconds, out vector transform[4])
	{
		vector rotation[3];
		Math3D.AnglesToMatrix(Vector(m_vDirection.ToYaw(), 0, 0), rotation);
		transform[0] = rotation[0];
		transform[1] = rotation[1];
		transform[2] = rotation[2];
		transform[3] = GetPosition(elapsedSeconds);
	}
}
