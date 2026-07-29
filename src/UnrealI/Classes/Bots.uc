//=============================================================================
// Bots
//=============================================================================
class Bots expands ScriptedPawn
	abstract;

var(Sounds) sound 	drown;
var(Sounds) sound	breathagain;
var(Sounds) sound	Footstep1;
var(Sounds) sound	Footstep2;
var(Sounds) sound	Footstep3;
var(Sounds) sound	HitSound3;
var(Sounds) sound	HitSound4;
var(Sounds) sound	Die2;
var(Sounds) sound	Die3;
var(Sounds) sound	Die4;
var(Sounds) sound	GaspSound;
var(Sounds) sound	UWHit1;
var(Sounds) sound	UWHit2;
var(Sounds) sound   LandGrunt;

var bool bNoShootDecor;
var bool bGathering;
var bool bCamping;
var Weapon EnemyDropped;
var float PlayerKills;
var float PlayerDeaths;
var float LastInvFind;

// Modern AI - Phase 1: perception & memory (x64 port).
// Opt-in via [UnrealI.Bots] bModernAI=True; default preserves 1998 behavior.
var() globalconfig bool bModernAI;      // modern perception/memory model
var() globalconfig bool bModernAIDebug; // log perception events
var float AcquireTime;                  // when the current enemy contact began
var float ReactionDelay;                // per-contact reaction latency (skill-scaled)
var vector BeliefPos;                   // believed enemy position at BeliefTime
var vector BeliefVel;                   // believed enemy velocity at BeliefTime
var float BeliefTime;                   // when the belief was last refreshed by real contact
var float BeliefConfidence;             // 1=clear sight, 0.5=sound only
var float RetreatUntil;                 // Phase 2: committed-resupply window - Retreating
                                        // holds until this time even without ATTITUDE_Fear
// Phase 3: independent skill axes (0-3). Each bot derives them from Skill
// plus a stable personality bias rolled once per life - so one bot can be a
// great shot but a poor mover, another the reverse. AdjustSkill (dynamic
// difficulty) shifts Skill; the biases keep the personality.
var float AimSkill;                     // aim convergence, tracking, leading
var float MoveSkill;                    // dodging and strafing
var float TacticSkill;                  // decision quality (feeds the Phase 2 scorer)
var float ReactSkill;                   // reaction latency (feeds the Phase 1 gate)
var float AimBias, MoveBias, TacticBias, ReactBias;
var bool bAxesInit;
// Phase 4: map economy - believed respawn clocks for the map's key pickups
// (armor, super items, top weapons). Clocks update only from what the bot
// actually experiences: taking an item itself, or seeing the spot.
var Inventory KeyItem[8];               // the map's most valuable respawning pickups
var float KeyRespawnAt[8];              // believed next-available time (0 = unknown)
var int NumKeyItems;
var bool bKeyItemsInit;

function eAttitude AttitudeTo(Pawn Other)
{
	if (Other.bIsPlayer)
	{
		if ( Level.Game.IsA('TeamGame') && (Team == Other.Team) )
			return ATTITUDE_Friendly;
		else 
		{
			if (RelativeStrength(Other) > Aggressiveness)
				AttitudeToPlayer = ATTITUDE_Fear;
			else if (AttitudeToPlayer == ATTITUDE_Fear)
				AttitudeToPlayer = ATTITUDE_Hate;
		}
		return AttitudeToPlayer;
	}
	else 
		return Super.AttitudeTo(Other);
}

//-----------------------------------------------------------------------------
// Sound functions 

function AdjustSkill(bool bWinner)
{
	if ( bWinner )
	{
		PlayerKills += 1;
		skill -= 1/Min(PlayerKills, 20);
		skill = FClamp(skill, 0, 3);
	}
	else
	{
		PlayerDeaths += 1;
		skill += 1/Min(PlayerDeaths, 20);
		skill = FClamp(skill, 0, 3);
	}
}

simulated function PlayFootStep()
{
	local sound step;
	local float decision;

	if ( Role < ROLE_Authority )
		return;
	if ( FootRegion.Zone.bWaterZone )
	{
		PlaySound(sound 'LSplash', SLOT_Interact, 1, false, 1500.0, 1.0);
		return;
	}

	decision = FRand();
	if ( decision < 0.34 )
		step = Footstep1;
	else if (decision < 0.67 )
		step = Footstep2;
	else
		step = Footstep3;

	if ( DesiredSpeed <= 0.5 )
		PlaySound(step, SLOT_Interact, 0.5, false, 400.0, 1.0);
	else 
		PlaySound(step, SLOT_Interact, 1, false, 1200.0, 1.0);
}

function PlayDyingSound()
{
	local float rnd;

	if ( HeadRegion.Zone.bWaterZone )
	{
		if ( FRand() < 0.5 )
			PlaySound(UWHit1, SLOT_Pain,2.0,,,Frand()*0.2+0.9);
		else
			PlaySound(UWHit2, SLOT_Pain,2.0,,,Frand()*0.2+0.9);
		return;
	}

	rnd = FRand();
	if (rnd < 0.25)
		PlaySound(Die, SLOT_Talk,2.0);
	else if (rnd < 0.5)
		PlaySound(Die2, SLOT_Talk,2.0);
	else if (rnd < 0.75)
		PlaySound(Die3, SLOT_Talk,2.0);
	else 
		PlaySound(Die4, SLOT_Talk,2.0);
}

function PlayTakeHitSound(int damage, name damageType, int Mult)
{
	if ( Level.TimeSeconds - LastPainSound < 0.25 )
		return;
	LastPainSound = Level.TimeSeconds;

	if ( HeadRegion.Zone.bWaterZone )
	{
		if ( damageType == 'Drowned' )
			PlaySound(drown, SLOT_Pain, 1.5);
		else if ( FRand() < 0.5 )
			PlaySound(UWHit1, SLOT_Pain,2.0,,,Frand()*0.15+0.9);
		else
			PlaySound(UWHit2, SLOT_Pain,2.0,,,Frand()*0.15+0.9);
		return;
	}
	damage *= FRand();

	if (damage < 8) 
		PlaySound(HitSound1, SLOT_Pain,2.0,,,Frand()*0.2+0.9);
	else if (damage < 25)
	{
		if (FRand() < 0.5) PlaySound(HitSound2, SLOT_Pain,2.0,,,Frand()*0.15+0.9);			
		else PlaySound(HitSound3, SLOT_Pain,2.0,,,Frand()*0.15+0.9);
	}
	else
		PlaySound(HitSound4, SLOT_Pain,2.0,,,Frand()*0.15+0.9);			
}

function CallForHelp()
{
	local Pawn P;

	P = Level.PawnList;
	while ( P != None )
	{
		if ( P.IsA('Bots') && (P.Team == Team) )
			Bots(P).HandleHelpMessageFrom(self);
		P = P.nextPawn;
	}
}

// (HandleHelpMessageFrom implemented below - Phase 4 team intel; x64 port)

function Gasp()
{
	if ( PainTime < 2 )
		PlaySound(GaspSound, SLOT_Talk, 2.0);
	else
		PlaySound(BreathAgain, SLOT_Talk, 2.0);
}

function PlayAcquisitionSound()
{
}

function PlayFearSound()
{
}

function PlayRoamingSound()
{
}

function PlayThreateningSound()
{
}

//-----------------------------------------------------------------------------
// Bot functions

function string[64] KillMessage(name damageType, pawn Other)
{
	return ( Level.Game.PlayerKillMessage(damageType, Other)$PlayerName );
}

function PreBeginPlay()
{
	bIsPlayer = true;
	if (Orders == '')
		Orders = 'Roaming';
	Super.PreBeginPlay();
}
	
function SetFall()
{
	if (Enemy != None)
	{
		NextState = 'Attacking'; //default
		NextLabel = 'Begin';
		TweenToFalling();
		NextAnim = AnimSequence;
		GotoState('FallingState');
	}
}

event UpdateEyeHeight(float DeltaTime)
{
	local float smooth, bound;
	
	smooth = FMin(1.0, 10.0 * DeltaTime/Level.TimeDilation);
	// smooth up/down stairs
	If ( (Physics == PHYS_Walking) && !bJustLanded)
	{
		EyeHeight = (EyeHeight - Location.Z + OldLocation.Z) * (1 - smooth) + BaseEyeHeight * smooth;
		bound = -0.5 * CollisionHeight;
		if (EyeHeight < bound)
			EyeHeight = bound;
		else
		{
			bound = CollisionHeight + FMin(FMax(0.0,(OldLocation.Z - Location.Z)), MaxStepHeight); 
			 if ( EyeHeight > bound )
				EyeHeight = bound;
		}
	}
	else
	{
		smooth = FMax(smooth, 0.35); //FIXME - was 0.43, what should it be?
		bJustLanded = false;
		EyeHeight = EyeHeight * ( 1 - smooth) + BaseEyeHeight * smooth;
	}

	// also update viewrotation
	ViewRotation = Rotation;
}

/* Adjust hit location - adjusts the hit location in for pawns, and returns
true if it was really a hit, and false if not (for ducking, etc.)
*/
function bool AdjustHitLocation(out vector HitLocation, vector TraceDir)
{
	local float adjZ, maxZ;

	TraceDir = Normal(TraceDir);
	HitLocation = HitLocation + 0.5 * CollisionRadius * TraceDir;
	if ( BaseEyeHeight == Default.BaseEyeHeight )
		return true;

	maxZ = Location.Z + EyeHeight + 0.25 * CollisionHeight;
	if ( HitLocation.Z > maxZ )
	{
		if ( TraceDir.Z >= 0 )
			return false;
		adjZ = (maxZ - HitLocation.Z)/TraceDir.Z;
		HitLocation.Z = maxZ;
		HitLocation.X = HitLocation.X + TraceDir.X * adjZ;
		HitLocation.Y = HitLocation.Y + TraceDir.Y * adjZ;
		if ( VSize(HitLocation - Location) > CollisionRadius )	
			return false;
	}
	return true;
}

function bool CanFireAtEnemy()
{
	local vector HitLocation, HitNormal,X,Y,Z, projStart;
	local actor HitActor;
	
	if ( Weapon == None )
		return false;
	
	GetAxes(Rotation,X,Y,Z);
	projStart = Location + Weapon.CalcDrawOffset() + Weapon.FireOffset.X * X + 1.2 * Weapon.FireOffset.Y * Y + Weapon.FireOffset.Z * Z;
	if ( Weapon.IsA('ASMD') || Weapon.IsA('Minigun') || Weapon.IsA('Rifle') ) //instant hit
		HitActor = Trace(HitLocation, HitNormal, Enemy.Location + Enemy.CollisionHeight * vect(0,0,0.7), projStart, true);
	else
		HitActor = Trace(HitLocation, HitNormal, 
				projStart + 220 * Normal(Enemy.Location + Enemy.CollisionHeight * vect(0,0,0.7) - Location), 
				projStart, true);

	if ( HitActor == Enemy )
		return true;
	if ( (HitActor != None) && (VSize(HitLocation - Location) < 200) )
		return false;
	if ( (Pawn(HitActor) != None) && (AttitudeTo(Pawn(HitActor)) > ATTITUDE_Ignore) )
		return false;

	return true;
}

function bool Gibbed()
{
	return ( (Health < -80) || ((Health < -40) && (FRand() < 0.65)) );
}

function ChangedWeapon()
{
	local int usealt;

	if ( Weapon == PendingWeapon )
	{
		if ( Weapon == None )
			SwitchToBestWeapon();
		else if ( Weapon.GetStateName() == 'DownWeapon' ) 
			Weapon.GotoState('Idle');
		PendingWeapon = None;
	}
	else
		Super.ChangedWeapon();

	if ( Weapon != None )
	{
		if ( (bFire > 0) || (bAltFire > 0) )
		{
			Weapon.RateSelf(usealt);
			if ( usealt == 0 )
			{
				bAltFire = 0;
				bFire = 1;
				Weapon.Fire(1.0);
			}
			else
			{
				bAltFire = 0;
				bFire = 1;
				Weapon.AltFire(1.0);
			}
		}
		Weapon.SetHand(0);
	}
}

function PreSetMovement()
{
	if ( Skill == 3 )
	{
		PeripheralVision = -0.1;
		RotationRate.Yaw = 100000;
	}
	else
	{
		PeripheralVision = 0.7 - 0.35 * skill;
		RotationRate.Yaw = 30000 + 16000 * skill;
	}
	if (JumpZ > 0)
		bCanJump = true;
	bCanWalk = true;
	bCanSwim = true;
	bCanFly = false;
	MinHitWall = -0.5;
	bCanOpenDoors = true;
	bCanDoSpecial = true;
	if ( skill <= 1 )
	{
		bCanDuck = false;
		MaxDesiredSpeed = 0.8 + 0.1 * skill;
	}
	else
	{
		MaxDesiredSpeed = 1;
		bCanDuck = true;
	}
}

function PainTimer()
{
	local float depth;
	if (Health < 0)
		return;

	if (FootRegion.Zone.bPainZone)
		Super.PainTimer();
	else if (HeadRegion.Zone.bWaterZone)
	{
		if (bDrowning)
			self.TakeDamage(5, None, Location, vect(0,0,0), 'drowned'); 
		else
		{
			bDrowning = true;
			GotoState('FindAir');
		}
		if (Health > 0)
			PainTime = 2.0;
	}
}	

function AnnoyedBy(Pawn Other)
{
}

function TryToDuck(vector duckDir, bool bReversed)
{
	local vector HitLocation, HitNormal, Extent;
	local actor HitActor;
	local bool bSuccess, bDuckLeft;

	//log("duck");			
	duckDir.Z = 0;
	bDuckLeft = !bReversed;
	Extent.X = CollisionRadius;
	Extent.Y = CollisionRadius;
	Extent.Z = CollisionHeight;
	HitActor = Trace(HitLocation, HitNormal, Location + 240 * duckDir, Location, false, Extent);
	bSuccess = ( (HitActor == None) || (VSize(HitLocation - Location) > 150) );
	if ( !bSuccess )
	{
		bDuckLeft = !bDuckLeft;
		duckDir *= -1;
		HitActor = Trace(HitLocation, HitNormal, Location + 240 * duckDir, Location, false, Extent);
		bSuccess = ( (HitActor == None) || (VSize(HitLocation - Location) > 150) );
	}
	if ( !bSuccess )
		return;
	
	if ( HitActor == None )
		HitLocation = Location + 240 * duckDir; 

	HitActor = Trace(HitLocation, HitNormal, HitLocation - MaxStepHeight * vect(0,0,1), HitLocation, false, Extent);
	if (HitActor == None)
		return;
		
	//log("good duck");

	SetFall();
	Velocity = duckDir * 400;
	Velocity.Z = 160;
	PlayDodge(bDuckLeft);
	SetPhysics(PHYS_Falling);
	GotoState('FallingState','Ducking');
}

function PlayDodge(bool bDuckLeft)
{
	PlayDuck();
}

//FIXME - here decide when to pause/start firing based on weapon,etc
function PlayCombatMove()
{	
	PlayRunning();
	if ( skill > 2 )
		bReadyToAttack = true;
	if ( bMovingRangedAttack && bReadyToAttack && bCanFire )
	{
		if ( NeedToTurn(Enemy.Location) )
		{
			bAltFire = 0;
			bFire = 0;
		}
		else 
			FireWeapon(); 
	}		
	else 
	{
		bFire = 0;
		bAltFire = 0;
	}
}

function PlayMeleeAttack()
{
	//log("play melee attack");
	Acceleration = AccelRate * VRand();
	TweenToWaiting(0.15); 
	FireWeapon();
}

function PlayRangedAttack()
{
	//log("play ranged attack");
	TweenToWaiting(0.11);
	FireWeapon();
}

function PlayMovingAttack()
{
	PlayRunning();
	FireWeapon();
}

function PlayOutOfWater()
{
	PlayDuck();
}

function FireWeapon()
{
	local bool bUseAltMode;

	// Modern AI: still inside the reaction window after fresh contact - hold
	// fire (movement and positioning are unaffected). (x64 port)
	if ( bModernAI && (Level.TimeSeconds - AcquireTime < ReactionDelay) )
	{
		bFire = 0;
		bAltFire = 0;
		return;
	}

	bUseAltMode = SwitchToBestWeapon();
	bShootSpecial = false;


	if( Weapon!=None )
	{
		if ( (Weapon.AmmoType != None) && (Weapon.AmmoType.AmmoAmount <= 0) )
		{
			bReadyToAttack = true;
			return;
		}

		ViewRotation = Rotation;
		if ( bUseAltMode )
		{
			bFire = 0;
			bAltFire = 1;
			Weapon.AltFire(1.0);
		}
		else
		{
			bFire = 1;
			bAltFire = 0;
			Weapon.Fire(1.0);
		}
		PlayFiring();
	}
	if ( Target != Enemy )
		Target = Enemy;
}

function WhatToDoNext(name LikelyState, name LikelyLabel)
{
	bFire = 0;
	bAltFire = 0;
	bReadyToAttack = false;
	Enemy = None;
	if ( OldEnemy != None )
	{
		Enemy = OldEnemy;
		OldEnemy = None;
		GotoState('Attacking');
	}
	else if ( (LikelyState != 'Waiting') && (LikelyState != '') )
		GotoState(LikelyState, LikelyLabel);
	else
	{
		OrderObject = None;
		OrderTag = '';
		GotoState('Roaming');
		if ( Skill > 2.7 )
			bReadyToAttack = true; 
	}
}

function Killed(pawn Killer, pawn Other, name damageType)
{
	if (Other == Enemy)
	{
		bFire = 0;
		bAltFire = 0;
		bReadyToAttack = ( skill > 3 * FRand() );
		EnemyDropped = Enemy.Weapon;
		Enemy = None;
		GotoState('Attacking');
	}
	//log(Other$" killed");
}	

function ReSetSkill()
{
	bLeadTarget = (1.5 * FRand() < Skill);
	if ( Skill == 0 )
	{
		Health = 80;
		ReFireRate = 0.75 * Default.ReFireRate;
	}
	else
		ReFireRate = Default.ReFireRate * (1 - 0.25 * skill);

	// Modern AI Phase 3: derive the per-axis skill profile. Biases are rolled
	// once per bot so its personality survives respawns and AdjustSkill.
	if ( !bAxesInit )
	{
		bAxesInit = true;
		AimBias    = 1.4 * (FRand() - 0.5);
		MoveBias   = 1.4 * (FRand() - 0.5);
		TacticBias = 1.4 * (FRand() - 0.5);
		ReactBias  = 1.4 * (FRand() - 0.5);
	}
	AimSkill    = FClamp(Skill + AimBias, 0, 3);
	MoveSkill   = FClamp(Skill + MoveBias, 0, 3);
	TacticSkill = FClamp(Skill + TacticBias, 0, 3);
	ReactSkill  = FClamp(Skill + ReactBias, 0, 3);
	if ( bModernAI && bModernAIDebug )
		log("ModernAI: "$PlayerName$" axes aim="$AimSkill$" move="$MoveSkill$" tactic="$TacticSkill$" react="$ReactSkill);

	PreSetMovement();
}

//-----------------------------------------------------------------------------
// Modern AI - Phase 1: perception & memory (x64 port).
// The 1998 model reacts instantly and remembers only LastSeenPos. This adds:
//  - a skill-scaled reaction latency between first contact and opening fire
//  - a short-term enemy belief (position + velocity + confidence) fed by
//    sight, sound, and pain, projected forward when hunting.
// ScriptedPawn (the monsters) is untouched; everything gates on bModernAI.

function bool SetEnemy( Pawn NewEnemy )
{
	local Pawn Prev;
	local bool result;

	Prev = Enemy;
	result = Super.SetEnemy(NewEnemy);
	if ( bModernAI && result && (Enemy != None) && (Enemy != Prev) )
	{
		// Fresh contact: impose a human-ish reaction delay before firing.
		// (Phase 3: driven by the reaction axis, not the global scalar.)
		AcquireTime = Level.TimeSeconds;
		ReactionDelay = FMax(0.1, 0.5 - 0.13 * ReactSkill + 0.15 * FRand());
		if ( CanSee(Enemy) )
			RefreshBelief(1.0);
		// Phase 4: share fresh contacts with teammates.
		if ( Level.Game.IsA('TeamGame') )
			CallForHelp();
		if ( bModernAIDebug )
			log("ModernAI: "$PlayerName$" acquired "$Enemy.PlayerName$" reaction="$ReactionDelay);
	}
	return result;
}

function RefreshBelief(float Confidence)
{
	if ( Enemy == None )
		return;
	BeliefPos = LastSeenPos;
	if ( Confidence >= 1.0 )
		BeliefVel = Enemy.Velocity;
	else
		BeliefVel = vect(0,0,0); // heard/felt, not seen - no velocity knowledge
	BeliefTime = Level.TimeSeconds;
	BeliefConfidence = Confidence;
}

// Called from combat states at the moment line of sight is lost; the engine
// has kept LastSeenPos current until now, so snapshot position + velocity.
function NoteEnemyLost()
{
	if ( !bModernAI || (Enemy == None) )
		return;
	BeliefPos = LastSeenPos;
	BeliefVel = Enemy.Velocity;
	BeliefTime = Level.TimeSeconds;
	BeliefConfidence = 1.0;
	if ( bModernAIDebug )
		log("ModernAI: "$PlayerName$" lost sight of "$Enemy.PlayerName);
}

// Where the enemy probably is now: project the belief along its last known
// velocity, bounded by confidence and clamped to the first wall hit.
function vector PredictedEnemyPos()
{
	local vector Projected, HitLocation, HitNormal;
	local float dt;
	local actor HitActor;

	if ( BeliefTime <= 0 )
		return LastSeenPos;
	dt = FMin(Level.TimeSeconds - BeliefTime, 0.5 + BeliefConfidence);
	Projected = BeliefPos + dt * BeliefVel;
	if ( Projected != BeliefPos )
	{
		HitActor = Trace(HitLocation, HitNormal, Projected, BeliefPos, false);
		if ( HitActor != None )
			Projected = HitLocation + CollisionRadius * HitNormal;
	}
	return Projected;
}

function HearNoise(float Loudness, Actor NoiseMaker)
{
	Super.HearNoise(Loudness, NoiseMaker);
	if ( bModernAI && (Enemy != None) && (NoiseMaker != None)
		&& (NoiseMaker.instigator == Enemy) && !CanSee(Enemy) )
		RefreshBelief(0.5); // sound refreshed the (fuzzy) LastSeenPos estimate
}

function TakeDamage( int Damage, Pawn instigatedBy, Vector hitlocation,
						Vector momentum, name damageType)
{
	Super.TakeDamage(Damage, instigatedBy, hitlocation, momentum, damageType);
	if ( bModernAI && (Health > 0) && (instigatedBy != None) && (instigatedBy == Enemy) )
	{
		// Pain cuts the reaction delay short and betrays a rough position.
		AcquireTime = FMin(AcquireTime, Level.TimeSeconds - ReactionDelay);
		if ( !CanSee(Enemy) )
		{
			BeliefPos = Enemy.Location + 150 * FRand() * VRand();
			BeliefVel = vect(0,0,0);
			BeliefTime = Level.TimeSeconds;
			BeliefConfidence = 0.6;
		}
	}
}

//-----------------------------------------------------------------------------
// Modern AI - Phase 2: utility-based combat decisions (x64 port).
// Replaces the Attacking.ChooseAttackMode() if/FRand cascade for DM combat
// (non-player enemies keep the 1998 router). Every option is an existing FSM
// state, scored from health, ammo, relative strength, aggression, and the
// Phase 1 enemy belief; the highest score dispatches. bModernAIDebug logs the
// full score vector for every decision, so choices are explainable.

function float AmmoFraction()
{
	if ( Weapon == None )
		return 0;
	if ( Weapon.AmmoType == None )
		return 1; // ammo-less weapon never runs dry
	return FClamp(Weapon.AmmoType.AmmoAmount / FMax(1.0, Weapon.AmmoType.MaxAmmo), 0, 1);
}

function ModernChooseAttackMode()
{
	local float SEngage, STactical, SRetreat, SHunt, SStakeOut;
	local float Dist, Strength, HealthFrac, AmmoFrac, BeliefFresh;
	local bool bSeeEnemy, bFearEnemy;
	local pawn changeEn;

	// Terminal cases, exactly as the 1998 router.
	if ( (Enemy == None) || (Enemy.Health <= 0) )
	{
		if ( Orders == 'Attacking' )
			Orders = '';
		WhatToDoNext('','');
		return;
	}
	if ( AttitudeTo(Enemy) == ATTITUDE_Friendly )
	{
		WhatToDoNext('',''); // teammate - never fight
		return;
	}

	bSeeEnemy = LineOfSightTo(Enemy);

	// Keep the 1998 enemy swap: current enemy hidden but the old one visible.
	if ( !bSeeEnemy && (OldEnemy != None) && (AttitudeTo(OldEnemy) == ATTITUDE_Hate)
		&& LineOfSightTo(OldEnemy) )
	{
		changeEn = Enemy;
		Enemy = OldEnemy;
		OldEnemy = changeEn;
		bSeeEnemy = true;
	}

	// Decision inputs.
	Dist = VSize(Enemy.Location - Location);
	Strength = FClamp(RelativeStrength(Enemy), -1, 1); // >0 = enemy stronger
	HealthFrac = FClamp(Health / 100.0, 0, 1);
	AmmoFrac = AmmoFraction();
	bFearEnemy = ( AttitudeTo(Enemy) == ATTITUDE_Fear );
	BeliefFresh = BeliefConfidence * FClamp(1 - (Level.TimeSeconds - BeliefTime) / 4, 0, 1);

	if ( bSeeEnemy )
	{
		// Engage: stand and deliver (melee/ranged attack states). Wants
		// readiness, a clear shot, health/ammo, and a winnable matchup;
		// higher tactical skill prefers to keep moving instead. (Phase 3)
		SEngage = 0.35 + 0.25 * HealthFrac + 0.15 * AmmoFrac - 0.30 * Strength
					+ 0.20 * Aggressiveness - 0.10 * TacticSkill + 0.15 * FRand();
		if ( !bReadyToAttack || !CanFireAtEnemy() )
			SEngage = 0;

		// Reposition: strafe and fire on the move, grabbing nearby items -
		// the bread and butter of deathmatch; tactical skill strafes more.
		STactical = 0.55 + 0.10 * TacticSkill + 0.15 * FRand();

		// Retreat/resupply: driven by low health, low ammo, and fear.
		SRetreat = 0.55 * (1 - HealthFrac) + 0.45 * (1 - AmmoFrac)
					+ 0.35 * FClamp(Strength, 0, 1) - 0.15 * Aggressiveness;
		if ( bFearEnemy )
			SRetreat += 0.35;

		SHunt = 0;
		SStakeOut = 0;
	}
	else
	{
		SEngage = 0;
		STactical = 0;

		// Hunt: chase the projected enemy position while the belief is fresh.
		SHunt = 0.40 + 0.35 * BeliefFresh + 0.15 * Aggressiveness
					- 0.20 * FClamp(Strength, 0, 1) + 0.10 * FRand();

		// Stake out: cover the last seen spot - best at close range where
		// re-contact is likely, pointless once the belief has gone stale.
		SStakeOut = 0.20 + 0.35 * BeliefFresh + 0.10 * FRand();
		if ( Dist < 700 )
			SStakeOut += 0.15;

		// Unseen is the safest time to break for health and ammo.
		SRetreat = 0.50 * (1 - HealthFrac) + 0.40 * (1 - AmmoFrac)
					+ 0.20 * FClamp(Strength, 0, 1);
		if ( bFearEnemy )
			SRetreat += 0.25;
	}

	if ( bModernAIDebug )
		log("ModernAI: "$PlayerName$" decide E="$SEngage$" T="$STactical$" R="$SRetreat$" H="$SHunt$" S="$SStakeOut);

	// Dispatch the winner onto the existing FSM states (retreat wins ties -
	// deliberate safety bias).
	Target = Enemy;
	if ( (SRetreat >= SEngage) && (SRetreat >= STactical)
		&& (SRetreat >= SHunt) && (SRetreat >= SStakeOut) )
	{
		RetreatUntil = Level.TimeSeconds + 3 + 2 * FRand();
		GotoState('Retreating');
	}
	else if ( bSeeEnemy && (SEngage >= STactical) )
	{
		if ( Dist <= MeleeRange + Enemy.CollisionRadius + CollisionRadius )
			GotoState('MeleeAttack');
		else
			GotoState('RangedAttack');
	}
	else if ( bSeeEnemy )
	{
		// Keep the 1998 fire cadence: a ready bot re-arms the refire timer
		// and shoots on the move from TacticalMove.
		if ( bReadyToAttack )
			SetTimer(TimeBetweenAttacks, false);
		GotoState('TacticalMove');
	}
	else if ( SHunt >= SStakeOut )
		GotoState('Hunting');
	else
	{
		HuntStartTime = Level.TimeSeconds;
		NumHuntPaths = 0;
		GotoState('StakeOut');
	}
}

//-----------------------------------------------------------------------------
// Modern AI - Phase 3: aim & movement skill models (x64 port).
// Replaces the single-curve aim jitter and flat dodge probability. The aim
// error converges while tracking a contact, only motion perpendicular to the
// line of fire is hard to track, leading is proportional (not a coin flip),
// and dodging reads the incoming weapon's threat through the movement axis.

function rotator AdjustAim(float projSpeed, vector projStart, int aimerror, bool leadTarget, bool warnTarget)
{
	local rotator FireRotation;
	local vector FireSpot, RelVel, DirToTarget;
	local actor HitActor;
	local vector HitLocation, HitNormal;
	local float Track, ModError, LeadFactor;
	local int ErrInt;

	if ( !bModernAI )
		return Super.AdjustAim(projSpeed, projStart, aimerror, leadTarget, warnTarget);

	if ( Target == None )
		Target = Enemy;
	if ( Target == None )
		return Rotation;
	if ( !Target.IsA('Pawn') )
		return rotator(Target.Location - Location);

	FireSpot = Target.Location;
	DirToTarget = Normal(Target.Location - Location);

	// Modern error model (replaces aimerror * (2.4 - 0.5*(skill+FRand()))):
	//  - convergence: error starts wide on a fresh contact and settles as the
	//    bot tracks it (anchored to the Phase 1 AcquireTime)
	//  - tracking: only motion perpendicular to the line of fire is hard
	//  - the weapon's aimerror parameter stays the per-weapon base error
	Track = FClamp(Level.TimeSeconds - AcquireTime, 0, 2.5);
	RelVel = Target.Velocity - Velocity;
	RelVel -= (RelVel Dot DirToTarget) * DirToTarget;
	ModError = aimerror * (1.5 - 0.36 * Track) * (1.9 - 0.44 * AimSkill)
				* (1 + VSize(RelVel) / 700) * (0.85 + 0.3 * FRand());

	// Modern leading: always lead a moving target, with proportional error -
	// poor aim under- or over-leads, good aim converges on the true solution.
	if ( projSpeed > 0 )
	{
		LeadFactor = 0.55 + 0.15 * AimSkill + 0.3 * (FRand() - 0.5) * (1 - AimSkill / 4);
		FireSpot += LeadFactor * Target.Velocity * VSize(Target.Location - ProjStart) / projSpeed;
		HitActor = Trace(HitLocation, HitNormal, FireSpot, ProjStart, false);
		if ( HitActor != None )
			FireSpot = 0.5 * (FireSpot + Target.Location);
	}

	// From here the 1998 logic is kept: splash-at-feet for good shots, trace
	// fallbacks (feet/middle/head), last-seen fallback with hold-fire, target
	// warning, and the turn-rate clamp on the final rotation.
	HitActor = self;
	if ( (Location.Z + 19 >= Target.Location.Z) && Target.IsA('Pawn')
		&& (Weapon != None) && Weapon.bSplashDamage && (0.5 * (AimSkill - 1) > FRand()) )
	{
 		HitActor = Trace(HitLocation, HitNormal, FireSpot - vect(0,0,80), FireSpot, false);
		if ( HitActor != None )
		{
			FireSpot = HitLocation + vect(0,0,3);
			HitActor = Trace(HitLocation, HitNormal, FireSpot, ProjStart, false);
		}
		else
			HitActor = self;
	}
	if ( HitActor != None )
	{
		FireSpot.Z = Target.Location.Z;
 		HitActor = Trace(HitLocation, HitNormal, FireSpot, ProjStart, false);
	}
	if ( HitActor != None )
	{
 		FireSpot.Z = Target.Location.Z + 0.9 * Target.CollisionHeight;
 		HitActor = Trace(HitLocation, HitNormal, FireSpot, ProjStart, false);
	}
	if ( (HitActor != None) && (Target == Enemy) )
	{
		FireSpot = LastSeenPos;
		if ( Location.Z >= LastSeenPos.Z )
			FireSpot.Z -= 0.5 * Enemy.CollisionHeight;
		if ( Weapon != None )
		{
	 		HitActor = Trace(HitLocation, HitNormal, FireSpot, ProjStart, false);
			if ( HitActor != None )
			{
				bFire = 0;
				bAltFire = 0;
				SetTimer(TimeBetweenAttacks, false);
			}
		}
	}

	FireRotation = Rotator(FireSpot - ProjStart);
	ErrInt = ModError;
	FireRotation.Yaw = FireRotation.Yaw + 0.5 * (Rand(2 * ErrInt) - ErrInt);
	if ( warnTarget && (Pawn(Target) != None) )
		Pawn(Target).WarnTarget(self, projSpeed, vector(FireRotation));

	FireRotation.Yaw = FireRotation.Yaw & 65535;
	if ( (Abs(FireRotation.Yaw - (Rotation.Yaw & 65535)) > 8192)
		&& (Abs(FireRotation.Yaw - (Rotation.Yaw & 65535)) < 57343) )
	{
		if ( (FireRotation.Yaw > Rotation.Yaw + 32768) ||
			((FireRotation.Yaw < Rotation.Yaw) && (FireRotation.Yaw > Rotation.Yaw - 32768)) )
			FireRotation.Yaw = Rotation.Yaw - 8192;
		else
			FireRotation.Yaw = Rotation.Yaw + 8192;
	}
	viewRotation = FireRotation;
	return FireRotation;
}

function WarnTarget(Pawn shooter, float projSpeed, vector FireDir)
{
	local float enemyDist, DodgeProb, Threat;
	local vector X, Y, Z, enemyDir;

	if ( !bModernAI )
	{
		Super.WarnTarget(shooter, projSpeed, FireDir);
		return;
	}
	if ( !bCanDuck || (Enemy == None) || (Physics == PHYS_Falling) )
		return;

	// Threat-based dodging (replaces the flat FRand() > 0.33*skill gate):
	// propensity comes from the movement axis, scaled by how dangerous the
	// incoming weapon actually is.
	Threat = 1.0;
	if ( (shooter != None) && (shooter.Weapon != None) )
		Threat = FClamp(shooter.Weapon.AIRating / 4.0, 0.6, 1.4);
	DodgeProb = (0.10 + 0.26 * MoveSkill) * Threat;
	if ( FRand() > DodgeProb )
		return;

	// Still needs enough flight time to react to the shot...
	if ( (shooter == None) || (projSpeed <= 0) )
		return;
	enemyDist = VSize(shooter.Location - Location);
	if ( enemyDist / projSpeed < 0.05 + 0.055 * (3 - MoveSkill) )
		return;

	// ...and to actually see the shooter.
	GetAxes(Rotation, X, Y, Z);
	enemyDir = (shooter.Location - Location) / enemyDist;
	if ( (enemyDir Dot X) < 0.8 )
		return;

	if ( (FireDir Dot Y) > 0 )
	{
		Y *= -1;
		TryToDuck(Y, true);
	}
	else
		TryToDuck(Y, false);
}

//-----------------------------------------------------------------------------
// Modern AI - Phase 4: map economy & team intel (x64 port).
// Bots track believed respawn clocks for the map's key pickups and travel to
// arrive as they come up, instead of only reacting to items they can see.
// Beliefs come only from experience: taking an item, or seeing its spot.
// Team layer: fresh contacts and retreats broadcast enemy intel to teammates
// through the (previously vestigial) CallForHelp/HandleHelpMessageFrom hooks.

function InitKeyItems()
{
	local Inventory Inv;
	local int i, worst;
	local float worstW;

	bKeyItemsInit = true;
	NumKeyItems = 0;
	foreach AllActors(class'Inventory', Inv)
	{
		if ( !Inv.bHeldItem && (Inv.MaxDesireability >= 0.75) && (Inv.RespawnTime > 0) )
		{
			if ( NumKeyItems < 8 )
			{
				KeyItem[NumKeyItems] = Inv;
				KeyRespawnAt[NumKeyItems] = 0;
				NumKeyItems++;
			}
			else
			{
				worst = 0;
				worstW = KeyItem[0].MaxDesireability;
				for ( i=1; i<8; i++ )
					if ( KeyItem[i].MaxDesireability < worstW )
					{
						worst = i;
						worstW = KeyItem[i].MaxDesireability;
					}
				if ( Inv.MaxDesireability > worstW )
				{
					KeyItem[worst] = Inv;
					KeyRespawnAt[worst] = 0;
				}
			}
		}
	}
	if ( bModernAIDebug )
		log("ModernAI: "$PlayerName$" tracking "$NumKeyItems$" key items");
}

function bool AddInventory( inventory NewItem )
{
	local int i;
	local bool result;

	result = Super.AddInventory(NewItem);
	// Phase 4: the bot knows exactly when an item it just took will respawn.
	if ( bModernAI && result )
		for ( i=0; i<NumKeyItems; i++ )
			if ( (KeyItem[i] != None) && (KeyItem[i].Class == NewItem.Class)
				&& (VSize(KeyItem[i].Location - Location) < 120) )
				KeyRespawnAt[i] = Level.TimeSeconds + KeyItem[i].RespawnTime;
	return result;
}

// Refresh clocks for spots the bot can currently see: a visibly present item
// is available now; a visibly empty spot means someone else took it.
function UpdateKeyItemBeliefs()
{
	local int i;

	if ( !bKeyItemsInit )
		InitKeyItems();
	for ( i=0; i<NumKeyItems; i++ )
		if ( (KeyItem[i] != None) && (VSize(KeyItem[i].Location - Location) < 1200)
			&& LineOfSightTo(KeyItem[i]) )
		{
			if ( KeyItem[i].IsInState('PickUp') )
				KeyRespawnAt[i] = Level.TimeSeconds;
			else if ( KeyRespawnAt[i] < Level.TimeSeconds )
				KeyRespawnAt[i] = Level.TimeSeconds + 0.5 * KeyItem[i].RespawnTime;
		}
}

// The best key pickup worth traveling for: available now, or up by arrival
// (plus a short camping window).
function Inventory BestTimedPickup()
{
	local int i;
	local float Wait, Travel, S, BestS;
	local Inventory Best;

	if ( !bKeyItemsInit )
		InitKeyItems();
	BestS = 0;
	for ( i=0; i<NumKeyItems; i++ )
		if ( KeyItem[i] != None )
		{
			Travel = VSize(KeyItem[i].Location - Location) / FMax(GroundSpeed, 100);
			Wait = FMax(0, KeyRespawnAt[i] - Level.TimeSeconds - Travel);
			if ( Wait < 2.5 )
			{
				S = KeyItem[i].BotDesireability(self) / (2 + Wait + Travel);
				if ( S > BestS )
				{
					BestS = S;
					Best = KeyItem[i];
				}
			}
		}
	return Best;
}

// Phase 4 team layer: adopt a teammate's reported contact as a
// low-confidence belief. (The stock Roaming state version additionally
// closes distance to help; this covers every other state.)
function HandleHelpMessageFrom(Pawn Other)
{
	if ( bModernAI && Level.Game.IsA('TeamGame') && (Other.Team == Team)
		&& (Other.Enemy != None) && (Enemy == None) )
	{
		if ( SetEnemy(Other.Enemy) )
		{
			LastSeenPos = Other.Enemy.Location;
			RefreshBelief(0.5);
		}
	}
}

//===============================================================================
// Bot states

state GameEnded
{
ignores SeePlayer, EnemyNotVisible, HearNoise, TakeDamage, Died, Bump, Trigger, HitWall, HeadZoneChange, FootZoneChange, ZoneChange, Falling, WarnTarget;

}

state Dying
{
ignores SeePlayer, EnemyNotVisible, HearNoise, Died, Bump, Trigger, HitWall, HeadZoneChange, FootZoneChange, ZoneChange, Falling, WarnTarget;

	function ReStartPlayer()
	{
		if( bHidden && Level.Game.RestartPlayer(self) )
		{
			Velocity = vect(0,0,0);
			Acceleration = vect(0,0,0);
			ViewRotation = Rotation;
			ReSetSkill();
			SetPhysics(PHYS_Falling);
			GotoState('Roaming');
		}
	}
	
	function TakeDamage( int Damage, Pawn instigatedBy, Vector hitlocation, 
							Vector momentum, name damageType)
	{
		if ( !bHidden )
			Super.TakeDamage(Damage, instigatedBy, hitlocation, momentum, damageType);
	}
	
	function BeginState()
	{
		SetTimer(0, false);
		Enemy = None;
		bFire = 0;
		bAltFire = 0;
	}

Begin:
	Sleep(0.2);
	if ( !bHidden )
	{
		SpawnCarcass();
		HidePlayer();
	}
TryAgain:
	Sleep(0.25 + DeathMatchGame(Level.Game).NumBots * FRand());
	ReStartPlayer();
	Goto('TryAgain');
}


state FallingState 
{
ignores Bump, Hitwall, HearNoise, WarnTarget;

	function Timer()
	{
		if ( Enemy != None )
		{
			bReadyToAttack = true;
			if ( CanFireAtEnemy() )
				GotoState('FallingState', 'FireWhileFalling');
		}
	}

	function Landed(vector HitNormal)
	{
		//Note - physics changes type to PHYS_Walking by default for landed pawns
		//log("Player landed w/ "$Velocity);
		PlayLanded(Velocity.Z);
		if (Velocity.Z < -1.4 * JumpZ)
		{
			MakeNoise(-0.5 * Velocity.Z/(FMax(JumpZ, 150.0)));
			if (Velocity.Z <= -1100)
			{
				if ( (Velocity.Z < -2000) && (ReducedDamageType != 'All') )
				{
					health = -1000; //make sure gibs
					Died(None, 'fell', Location);
				}
				else if ( Role == ROLE_Authority )
					TakeDamage(-0.15 * (Velocity.Z + 1050), None, Location, vect(0,0,0), 'fell');
			}
			GotoState('FallingState', 'Landed');
		}
		else 
			GotoState('FallingState', 'Done');
	}

	function BeginState()
	{
		Super.BeginState();
		if ( (bFire > 0) || (bAltFire > 0) || (Skill == 3) )
			SetTimer(0.01, false);
	}

FireWhileFalling:
	if ( Physics != PHYS_Falling )
		Goto('Done');
	TurnToward(Enemy);
	if ( CanFireAtEnemy() )
		FireWeapon();
	Sleep(0.9 + 0.2 * FRand());
	Goto('FireWhileFalling');
}			

state MeleeAttack
{
ignores SeePlayer, HearNoise, Bump;

	function KeepAttacking()
	{
		if ( (Enemy == None) || (Enemy.Health <= 0)
			|| (VSize(Enemy.Location - Location) > (0.9 * MeleeRange + Enemy.CollisionRadius + CollisionRadius)) ) 
			GotoState('Attacking');
		else 
		{
			bReadyToAttack = true;
			SetTimer(TimeBetweenAttacks, false);
			GotoState('TacticalMove', 'NoCharge');
		}
	}
	
	function BeginState()
	{
		Target = Enemy;
		Disable('AnimEnd');
		bCanJump = false;
	}
	
	function EndState()
	{
		bCanJump = (JumpZ > 0);
	}			
}

state RangedAttack
{
ignores SeePlayer, HearNoise;

	function KeepAttacking()
	{
		if ( bFiringPaused )
			return;
		if ( (Enemy == None) || (Enemy.Health <= 0) || !LineOfSightTo(Enemy) )
		{
			bFire = 0;
			bAltFire = 0; 
			GotoState('Attacking');
		}
		else if ( Skill > 3.5 * FRand() - 0.5 )
		{
			bReadyToAttack = true;
			GotoState('TacticalMove');
		}	
	}


	function AnimEnd()
	{
		local float decision;

		decision = FRand() - 0.27 * skill - 0.1;
		if ( (bFire == 0) && (bAltFire == 0) )
			decision = decision - 0.5;
		if ( decision < 0 )
			GotoState('RangedAttack', 'DoneFiring');
		else
		{
			PlayWaiting();
			FireWeapon();
		}
	}

	function BeginState()
	{
		Target = Enemy;
		Disable('AnimEnd');
		if ( bFiringPaused )
		{
			SetTimer(SpecialPause, false);
			SpecialPause = 0;
		}
	}
}

// Modern AI: the engine keeps LastSeenPos current while the enemy is visible
// and fires EnemyNotVisible at the moment sight is lost - snapshot the belief
// (position + velocity) right then, in each combat state. (x64 port)
state Attacking
{
ignores SeePlayer, HearNoise, Bump, HitWall;

	function EnemyNotVisible()
	{
		NoteEnemyLost();
		Super.EnemyNotVisible();
	}

	function ChooseAttackMode()
	{
		// Modern AI Phase 2: utility scorer for DM combat; monsters and
		// non-player enemies keep the 1998 cascade. (x64 port)
		if ( bModernAI && (Enemy != None) && Enemy.bIsPlayer )
			ModernChooseAttackMode();
		else
			Super.ChooseAttackMode();
	}
}

state Charging
{
ignores SeePlayer, HearNoise;

	function EnemyNotVisible()
	{
		NoteEnemyLost();
		Super.EnemyNotVisible();
	}
}

state Hunting
{
ignores EnemyNotVisible;

	function BeginState()
	{
		// Modern AI: hunt toward where the enemy is going, not where it was. (x64 port)
		if ( bModernAI )
		{
			LastSeenPos = PredictedEnemyPos();
			if ( bModernAIDebug )
				log("ModernAI: "$PlayerName$" hunting toward "$LastSeenPos);
		}
		Super.BeginState();
	}

	function AnimEnd()
	{
		PlayRunning();
		bFire = 0;
		bAltFire = 0;
		bReadyToAttack = true;
		Disable('AnimEnd');
	}


	function Bump(actor Other)
	{
		//log(Other.class$" bumped "$class);
		if (Pawn(Other) != None)
		{
			if (Enemy == Other)
				bReadyToAttack = True; //can melee right away
			SetEnemy(Pawn(Other));
			LastSeenPos = Enemy.Location;
		}
		setTimer(2.0, false);
		Disable('Bump');
	}
	
}

//FIXME - improve FindAir (use paths)
state FindAir
{
ignores SeePlayer, HearNoise, Bump;

	function HeadZoneChange(ZoneInfo newHeadZone)
	{
		Global.HeadZoneChange(newHeadZone);
		if (!newHeadZone.bWaterZone)
			GotoState('Attacking');
	}

	function TakeDamage( int Damage, Pawn instigatedBy, Vector hitlocation, 
						Vector momentum, name damageType)
	{
		Super.TakeDamage(Damage, instigatedBy, hitlocation, momentum, damageType);
		if ( health <= 0 )
			return;
		if ( NextState == 'TakeHit' )
		{
			NextState = 'FindAir'; 
			NextLabel = 'TakeHit';
			GotoState('TakeHit'); 
		}
	}

	function HitWall(vector HitNormal, actor Wall)
	{
		//change directions
		Destination = 200 * (Normal(Destination - Location) + HitNormal);
	}

	function AnimEnd() 
	{
		if (Enemy != None)
			PlayCombatMove();
		else
			PlayRunning();
	}

	function Timer()
	{
		bReadyToAttack = True;
		settimer(0.5, false);
	}

	function EnemyNotVisible()
	{
		////log("enemy not visible");
		bReadyToAttack = false;
	}

/* PickDestination()
*/
	function PickDestination(bool bNoCharge)
	{
		Destination = VRand();
		Destination.Z = 1;
		Destination = Location + 200 * Destination;				
	}

Begin:
	//log("Find air");
	TweenToRunning(0.2);
	Enable('AnimEnd');
	PickDestination(false);

DoMove:	
	if ( Enemy == None )
		MoveTo(Destination);
	else
	{
		bCanFire = true;
		StrafeFacing(Destination, Enemy);	
	}
	GotoState('Attacking');

TakeHit:
	TweenToRunning(0.15);
	Goto('DoMove');

}

state TacticalMove
{
ignores SeePlayer, HearNoise;

	function EnemyNotVisible()
	{
		NoteEnemyLost(); // Modern AI: snapshot belief at the loss edge (x64 port)
		if ( !bGathering && (aggressiveness > relativestrength(enemy)) )
		{
			if (ValidRecovery())
				GotoState('TacticalMove','RecoverEnemy');
			else
				GotoState('Attacking');
		}
		Disable('EnemyNotVisible');
	}

	function PickDestination(bool bNoCharge)
	{
		local inventory Inv, BestInv, SecondInv;
		local float Bestweight, NewWeight, MaxDist, SecondWeight;

		// possibly pick nearby inventory
		// higher skill bots will always strafe, lower skill
		// both do this less, and strafe less

		if ( !bReadyToAttack && (TimerRate == 0.0) )
			SetTimer(0.7, false);
		if ( LastInvFind - Level.TimeSeconds < 2.5 - 0.5 * skill )
		{
			Super.PickDestination(bNoCharge);
			return;
		}

		LastInvFind = Level.TimeSeconds;
		bGathering = false;
		BestWeight = 0;
		MaxDist = 600 + 70 * skill;
		foreach visiblecollidingactors(class'Inventory', Inv, MaxDist)
			if ( (Inv.IsInState('PickUp')) && (Inv.MaxDesireability/200 > BestWeight)
				&& (Inv.Location.Z < Location.Z + MaxStepHeight + CollisionHeight)
				&& (Inv.Location.Z > FMin(Location.Z, Enemy.Location.Z) - CollisionHeight) )
			{
				NewWeight = inv.BotDesireability(self)/VSize(Inv.Location - Location);
				if ( NewWeight > BestWeight )
				{
					SecondWeight = BestWeight;
					BestWeight = NewWeight;
					SecondInv = BestInv;
					BestInv = Inv;
				}
			}

		if ( BestInv == None )
		{
			Super.PickDestination(bNoCharge);
			return;
		}

		if ( TryToward(BestInv, BestWeight) )
			return;

		if ( SecondInv == None )
		{
			Super.PickDestination(bNoCharge);
			return;
		}

		if ( TryToward(SecondInv, SecondWeight) )
			return;

		Super.PickDestination(bNoCharge);
	}

	function bool TryToward(inventory Inv, float Weight)
	{
		local float s;

		if ( (Weight < 0.001) && ((Weight < 0.001 - 0.0002 * skill)
				|| !Enemy.LineOfSightTo(Inv)) )
			return false;

		if ( ActorReachable(Inv) )
		{
			Destination = Inv.Location;
			bGathering = true;
			// Phase 3: strafing quality comes from the movement axis. (x64 port)
			s = skill;
			if ( bModernAI )
				s = MoveSkill;
			if ( 2.7 * FRand() < s )
				GotoState('TacticalMove','DoStrafeMove');
			else
				GotoState('TacticalMove','DoDirectMove');
			return true;
		}

		return false;
	}

	function PainTimer()
	{
		if ( (FootRegion.Zone.bPainZone) && (FootRegion.Zone.DamagePerSec > 0) )
			GotoState('Retreating');
		Super.PainTimer();
	}

}

/* Retreating for a bot is going toward an item while still engaged with an enemy, but fearing that enemy (so
no desire to remain engaged)
   TacticalGet is for going to an item while engaged, and remaining engaged. TBD
   Roaming is going to items w/ no enemy. TBD
*/

state Retreating
{
ignores EnemyNotVisible;

	function SeePlayer(Actor SeenPlayer)
	{
		if ( (SeenPlayer == Enemy) || LineOfSightTo(Enemy) )
			return;
		if ( SetEnemy(Pawn(SeenPlayer)) )
		{
			LastSeenPos = SeenPlayer.Location;
			MakeNoise(1.0);
			GotoState('Attacking');
		}
	}

	function HearNoise(float Loudness, Actor NoiseMaker)
	{
		if ( (NoiseMaker.instigator == Enemy) || LineOfSightTo(Enemy) )
			return;

		if ( SetEnemy(NoiseMaker.instigator) )
		{
			LastSeenPos = 0.5 * (NoiseMaker.Location + VSize(NoiseMaker.Location - Location) * vector(Rotation));
			MakeNoise(1.0);
			GotoState('Attacking');
		}
	}

	function TakeDamage( int Damage, Pawn instigatedBy, Vector hitlocation, 
							Vector momentum, name damageType)
	{
		Global.TakeDamage(Damage, instigatedBy, hitlocation, momentum, damageType);
		if ( health <= 0 )
			return;
		if (NextState == 'TakeHit')
		{
			NextState = 'Retreating'; 
			NextLabel = 'TakeHit';
			GotoState('TakeHit'); 
		}
		else if ( !bCanFire && (skill > 3 * FRand()) )
			GotoState('Retreating', 'Moving');
	}

	function Timer()
	{
		bReadyToAttack = True;
		Enable('Bump');
	}
	
	function SetFall()
	{
		NextState = 'Retreating'; 
		NextLabel = 'Landed';
		NextAnim = AnimSequence;
		GotoState('FallingState'); 
	}

	function HitWall(vector HitNormal, actor Wall)
	{
		if (Physics == PHYS_Falling)
			return;
		if ( Wall.IsA('Mover') && Mover(Wall).HandleDoor(self) )
		{
			if ( SpecialPause > 0 )
				Acceleration = vect(0,0,0);
			GotoState('Retreating', 'SpecialNavig');
			return;
		}
		Focus = Destination;
		if (PickWallAdjust())
			GotoState('Retreating', 'AdjustFromWall');
		else
			MoveTimer = -1.0;
	}

	function PickDestination()
	{
	 	local inventory Inv, BestInv, SecondInv;
		local float Bestweight, NewWeight, invDist, MaxDist, SecondWeight;
		local actor BestPath;
		local bool bTriedFar;

		if ( !bReadyToAttack && (TimerRate == 0.0) )
			SetTimer(0.7, false);

		// do I still fear my enemy?
		// Modern AI Phase 2: a bot retreating to resupply (low health/ammo)
		// commits to the run for a few seconds even without ATTITUDE_Fear,
		// instead of ping-ponging straight back to Attacking. (x64 port)
		if ( (Enemy == None)
			|| ( (AttitudeTo(Enemy) > ATTITUDE_Fear)
				&& !(bModernAI && (Level.TimeSeconds < RetreatUntil)) ) )
		{
			GotoState('Attacking');
			return;
		}

		// Phase 4: a key pickup that is up (or up by arrival) is the best
		// resupply destination. (x64 port)
		if ( bModernAI )
		{
			UpdateKeyItemBeliefs();
			BestInv = BestTimedPickup();
			if ( (BestInv != None) && (BestInv.BotDesireability(self) > 0.4) )
			{
				if ( ActorReachable(BestInv) )
				{
					MoveTarget = BestInv;
					Destination = BestInv.Location;
					if ( bModernAIDebug )
						log("ModernAI: "$PlayerName$" retreating to timed pickup "$BestInv.Class);
					return;
				}
				if ( FindBestPathToward(BestInv) )
				{
					if ( bModernAIDebug )
						log("ModernAI: "$PlayerName$" retreat-pathing to timed pickup "$BestInv.Class);
					return;
				}
			}
			BestInv = None;
		}

		bestweight = 0;

		//first look at nearby inventory < 500 dist
		// FIXME reduce favoring of stuff nearer/visible to enemy
		MaxDist = 500 + 70 * skill;
		foreach visiblecollidingactors(class'Inventory', Inv, MaxDist)
			if ( (Inv.IsInState('PickUp')) && (Inv.MaxDesireability/200 > BestWeight)
				&& (Inv.Location.Z < Location.Z + MaxStepHeight + CollisionHeight)
				&& (Inv.Location.Z > FMin(Location.Z, Enemy.Location.Z) - CollisionHeight) )
			{
				NewWeight = inv.BotDesireability(self)/VSize(Inv.Location - Location);
				if ( NewWeight > BestWeight )
				{
					SecondWeight = BestWeight;
					BestWeight = NewWeight;
					SecondInv = BestInv;
					BestInv = Inv;
				}
			}

		 // see if better long distance inventory 
		if ( BestWeight < 0.2 )
		{ 
			bTriedFar = true;
			BestPath = FindBestInventoryPath(BestWeight, false);
			if ( BestPath != None )
			{
				MoveTarget = BestPath;
				return;
			}
		}

		 // if nothing, then tactical move
		if ( (BestInv != None) && ActorReachable(BestInv) )
		{
			MoveTarget = BestInv;
			return;
		}

		if ( (SecondInv != None) && ActorReachable(SecondInv) )
		{
			MoveTarget = BestInv;
			return;
		}
		if ( !bTriedFar )
		{ 
			BestWeight = 0;
			BestPath = FindBestInventoryPath(BestWeight, false);
			if ( BestPath != None )
			{
				MoveTarget = BestPath;
				return;
			}
		}

		LastInvFind = Level.TimeSeconds;
		GotoState('TacticalMove', 'NoCharge');
	}

	function Bump(actor Other)
	{
		local vector VelDir, OtherDir;
		local float speed;

		//log(Other.class$" bumped "$class);
		if (Pawn(Other) != None)
		{
			if ( (Other == Enemy) || SetEnemy(Pawn(Other)) )
			{
				bReadyToAttack = true;
				GotoState('Attacking');
			}
			return;
		}
		if ( TimerRate <= 0 )
			setTimer(1.0, false);
		speed = VSize(Velocity);
		if ( speed > 1 )
		{
			VelDir = Velocity/speed;
			VelDir.Z = 0;
			OtherDir = Other.Location - Location;
			OtherDir.Z = 0;
			OtherDir = Normal(OtherDir);
			if ( (VelDir Dot OtherDir) > 0.9 )
			{
				Velocity.X = VelDir.Y;
				Velocity.Y = -1 * VelDir.X;
				Velocity *= FMax(speed, 200);
			}
		}
		Disable('Bump');
	}

	function AnimEnd() 
	{
		if ( bCanFire && LineOfSightTo(Enemy) )
			PlayCombatMove();
		else
			PlayRunning();
	}

	function BeginState()
	{
		CallForHelp();
		bCanFire = false;
		SpecialGoal = None;
		SpecialPause = 0.0;
	}

Begin:
	//log(class$" retreating");
	if ( (TimerRate == 0.0) || (bReadyToAttack && (FRand() < 0.4)) )
	{
		SetTimer(TimeBetweenAttacks, false);
		bReadyToAttack = false;
	}
	TweenToRunning(0.15);
	WaitForLanding();
	
RunAway:
	PickDestination();
SpecialNavig:
	if (SpecialPause > 0.0)
	{
		if ( LineOfSightTo(Enemy) )
		{
			bFiringPaused = true;
			NextState = 'Retreating';
			NextLabel = 'Moving';
			GotoState('RangedAttack');
		}
		Disable('AnimEnd');
		Acceleration = vect(0,0,0);
		TweenToPatrolStop(0.3);
		Sleep(SpecialPause);
		SpecialPause = 0.0;
		Enable('AnimEnd');
		TweenToRunning(0.1);
	}
Moving:
	if ( MoveTarget == None )
	{
		Sleep(0.0);
		Goto('RunAway');
	}
	if ( MoveTarget.IsA('InventorySpot') 
		&& (InventorySpot(MoveTarget).markedItem.GetStateName() == 'Pickup') )
			MoveTarget = InventorySpot(MoveTarget).markedItem;
	if ( (skill < 3) && (!LineOfSightTo(Enemy) ||
		(Skill - 2 * FRand() + (Normal(Enemy.Location - Location - vect(0,0,1) * (Enemy.Location.Z - Location.Z)) 
			Dot Normal(MoveTarget.Location - Location - vect(0,0,1) * (MoveTarget.Location.Z - Location.Z))) < 0)) )
	{
		bCanFire = false;
		MoveToward(MoveTarget);
	}
	else
	{
		bCanFire = true;
		StrafeFacing(MoveTarget.Location, Enemy);
	}
	Goto('RunAway');

Landed:
	AnimEnd();
	Goto('Moving');

TakeHit:
	TweenToRunning(0.12);
	Goto('Moving');

AdjustFromWall:
	StrafeTo(Destination, Focus); 
	MoveTo(Destination);
	Goto('Moving');
}

state Roaming
{
ignores EnemyNotVisible;

	function HandleHelpMessageFrom(Pawn Other)
	{
		if ( (Health > 60) && (Weapon.AIRating > 3) && (Other.Team == Team)
			&& (Other.Enemy != None)
			&& (VSize(Other.Enemy.Location - Location) < 1500) )
		{
			SetEnemy(Other.Enemy);
			// Phase 4: the teammate's report seeds the enemy belief. (x64 port)
			if ( bModernAI && (Enemy != None) )
			{
				LastSeenPos = Enemy.Location;
				RefreshBelief(0.5);
			}
			GotoState('Attacking');
		}
	}

	function TakeDamage( int Damage, Pawn instigatedBy, Vector hitlocation, 
							Vector momentum, name damageType)
	{
		Global.TakeDamage(Damage, instigatedBy, hitlocation, momentum, damageType);
		if ( health <= 0 )
			return;
		if (NextState == 'TakeHit')
		{
			NextState = 'Attacking'; 
			NextLabel = '';
			GotoState('TakeHit'); 
		}
		else if ( !bCanFire && (skill > 3 * FRand()) )
			GotoState('Attacking');
	}

	function Timer()
	{
		bReadyToAttack = True;
		Enable('Bump');
	}
	
	function SetFall()
	{
		NextState = 'Roaming'; 
		NextLabel = 'Landed';
		NextAnim = AnimSequence;
		GotoState('FallingState'); 
	}

	function HitWall(vector HitNormal, actor Wall)
	{
		if (Physics == PHYS_Falling)
			return;
		if ( Wall.IsA('Mover') && Mover(Wall).HandleDoor(self) )
		{
			if ( SpecialPause > 0 )
				Acceleration = vect(0,0,0);
			GotoState('Roaming', 'SpecialNavig');
			return;
		}
		Focus = Destination;
		if (PickWallAdjust())
			GotoState('Roaming', 'AdjustFromWall');
		else
			MoveTimer = -1.0;
	}

	function PickDestination()
	{
		local inventory Inv, BestInv, KnowPath;
		local float Bestweight, NewWeight, DroppedDist;
		local actor BestPath;
		local actor HitActor;
		local vector HitNormal, HitLocation;
		local decoration Dec;

		if ( (EnemyDropped != None) && !EnemyDropped.bDeleteMe 
			&& (EnemyDropped.Owner == None) )
		{
			DroppedDist = VSize(EnemyDropped.Location - Location);
			if ( (DroppedDist < 800) && ActorReachable(EnemyDropped) )
			{
				BestWeight = EnemyDropped.BotDesireability(self); 		
				if ( BestWeight > 0.4 )
				{
					MoveTarget = EnemyDropped;
					EnemyDropped = None;
					return; 
				}
				BestInv = EnemyDropped;
				BestWeight = BestWeight/DroppedDist;
				KnowPath = BestInv;
			}	
			else
				BestWeight = 0;
		}	
		else
			BestWeight = 0;

		EnemyDropped = None;
									
		//first look at nearby inventory < 600 dist
		foreach visiblecollidingactors(class'Inventory', Inv, 600)
			if ( (Inv.IsInState('PickUp')) && (Inv.MaxDesireability/50 > BestWeight)
				&& (Inv.Location.Z < Location.Z + MaxStepHeight + CollisionHeight) )
			{
				NewWeight = inv.BotDesireability(self)/VSize(Inv.Location - Location);
				if ( NewWeight > BestWeight )
				{
					BestWeight = NewWeight;
					BestInv = Inv;
				}
			}

		if ( (BestInv != None) && ActorReachable(BestInv) )
		{
			MoveTarget = BestInv;
			return;
		}
		else if ( KnowPath != None )
		{
			MoveTarget = KnowPath;
			return;
		}

		// Phase 4: no immediate pickup - head for a key item that is (or will
		// be) up by arrival, using the believed respawn clocks. (x64 port)
		if ( bModernAI )
		{
			UpdateKeyItemBeliefs();
			Inv = BestTimedPickup();
			if ( (Inv != None) && (Inv.BotDesireability(self) > 0.4) )
			{
				if ( ActorReachable(Inv) )
				{
					MoveTarget = Inv;
					if ( bModernAIDebug )
						log("ModernAI: "$PlayerName$" roaming to timed pickup "$Inv.Class);
					return;
				}
				if ( FindBestPathToward(Inv) )
				{
					if ( bModernAIDebug )
						log("ModernAI: "$PlayerName$" pathing to timed pickup "$Inv.Class);
					return;
				}
			}
		}

		BestWeight = 0;

		// if none found, check for decorations with inventory
		if ( !bNoShootDecor )
			foreach visiblecollidingactors(class'Decoration', Dec, 500)
				if ( Dec.Contents != None )
				{
					bNoShootDecor = true;
					Target = Dec;
					GotoState('Roaming', 'ShootDecoration');
					return;
				}

		bNoShootDecor = false;

		// look for long distance inventory 
		BestPath = FindBestInventoryPath(BestWeight, (skill >= 2));
		if ( BestPath != None )
		{
			MoveTarget = BestPath;
			return;
		}

		 // if nothing, then wander or camp
		if ( FRand() < 0.35 )
			GotoState('Wandering');
		else
			GotoState('Roaming', 'Camp');
	}

	function Bump(actor Other)
	{
		local vector VelDir, OtherDir;
		local float speed;

		//log(Other.class$" bumped "$class);
		if (Pawn(Other) != None)
		{
			if ( (Other == Enemy) || SetEnemy(Pawn(Other)) )
			{
				bReadyToAttack = true;
				GotoState('Attacking');
			}
			return;
		}
		if ( TimerRate <= 0 )
			setTimer(1.0, false);
		speed = VSize(Velocity);
		if ( speed > 1 )
		{
			VelDir = Velocity/speed;
			VelDir.Z = 0;
			OtherDir = Other.Location - Location;
			OtherDir.Z = 0;
			OtherDir = Normal(OtherDir);
			if ( (VelDir Dot OtherDir) > 0.9 )
			{
				Velocity.X = VelDir.Y;
				Velocity.Y = -1 * VelDir.X;
				Velocity *= FMax(speed, 200);
			}
		}
		Disable('Bump');
	}

	function AnimEnd() 
	{
		if ( bCamping )
			PlayWaiting();
		else
			PlayRunning();
	}

	function BeginState()
	{
		bNoShootDecor = false;
		bCanFire = false;
		SpecialGoal = None;
		SpecialPause = 0.0;
	}

	function EndState()
	{
		Super.EndState();
		bCamping = false;
	}

Camp:
	bCamping = true;
	Acceleration = vect(0,0,0);
	TweenToWaiting(0.15);
	if ( NearWall(200) )
	{
		PlayTurning();
		TurnTo(Focus);
	}
	Sleep(3.5 + FRand() - skill);
	if ( (Weapon != None) && (Weapon.AIRating > 0.4) && (3 * FRand() > skill + 1) )
		Goto('Camp');
Begin:
	bCamping = false;
	TweenToRunning(0.1);
	WaitForLanding();
	
RunAway:
	PickDestination();
SpecialNavig:
	if (SpecialPause > 0.0)
	{
		Disable('AnimEnd');
		Acceleration = vect(0,0,0);
		TweenToPatrolStop(0.3);
		Sleep(SpecialPause);
		SpecialPause = 0.0;
		Enable('AnimEnd');
		TweenToRunning(0.1);
	}
Moving:
	if ( MoveTarget == None )
	{
		Acceleration = vect(0,0,0);
		Sleep(0.1);
		Goto('RunAway');
	}
	if ( MoveTarget.IsA('InventorySpot') )
	{
		if ( InventorySpot(MoveTarget).markedItem.GetStateName() == 'Pickup' )
			MoveTarget = InventorySpot(MoveTarget).markedItem;
		else if ( VSize(Location - MoveTarget.Location) < CollisionRadius )
			Goto('Camp');
	}
	bCamping = false;
	MoveToward(MoveTarget);
	Goto('RunAway');

TakeHit:
	TweenToRunning(0.12);
	Goto('Moving');

Landed:
	AnimEnd();
	Goto('Moving');

AdjustFromWall:
	bCamping = false;
	StrafeTo(Destination, Focus); 
	MoveTo(Destination);
	Goto('Moving');

ShootDecoration:
	TurnToward(Target);
	if ( Target != None )
	{
		FireWeapon();
		bAltFire = 0;
		bFire = 0;
	}
	Goto('RunAway');
}

state Wandering
{
ignores EnemyNotVisible;

Begin:
	//log(class$" Wandering");

Wander: 
	WaitForLanding();
	PickDestination();
	TweenToWalking(0.2);
	FinishAnim();
	PlayWalking();
	
Moving:
	Enable('HitWall');
	MoveTo(Destination, WalkingSpeed);
Pausing:
	Acceleration = vect(0,0,0);
	if ( NearWall(200) )
	{
		PlayTurning();
		TurnTo(Focus);
	}
	if (FRand() < 0.3)
		PlayRoamingSound();
	Enable('AnimEnd');
	NextAnim = '';
	TweenToPatrolStop(0.2);
	Sleep(1.0);
	Disable('AnimEnd');
	FinishAnim();
	GotoState('Roaming');

ContinueWander:
	FinishAnim();
	PlayWalking();
	if ( !bQuiet && (FRand() < 0.3) )
		PlayRoamingSound();
	if (FRand() < 0.2)
		Goto('Turn');
	Goto('Wander');

Turn:
	Acceleration = vect(0,0,0);
	PlayTurning();
	TurnTo(Location + 20 * VRand());
	Goto('Pausing');

AdjustFromWall:
	StrafeTo(Destination, Focus); 
	Destination = Focus; 
	Goto('Moving');
}

defaultproperties
{
     SightRadius=+03000.000000
     Aggressiveness=+00000.200000
     ReFireRate=+00000.900000
     bHasRangedAttack=True
     bMovingRangedAttack=True
     BaseEyeHeight=+00023.000000
     UnderWaterTime=+00020.000000
     bCanStrafe=True
	 bAutoActivate=True
     MeleeRange=+00050.000000
     Intelligence=BRAINS_HUMAN
     GroundSpeed=+00400.000000
     AirSpeed=+00400.000000
     AccelRate=+02048.000000
     MaxStepHeight=+00025.000000
     CombatStyle=+00000.20000
     DrawType=DT_Mesh
     LightBrightness=70
     LightHue=40
     LightSaturation=128
     LightRadius=6
	 bStasis=false
     Buoyancy=+00100.000000
     RotationRate=(Pitch=3072,Yaw=30000,Roll=2048)
     NetPriority=+00008.000000
}
