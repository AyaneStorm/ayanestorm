# Firestorm Remote Poser Permission Feasibility

## Conclusion

The required combination—arbitrary live poses, visible to all nearby viewers,
and no paid asset uploads—is not implementable by Firestorm alone on the normal
Second Life animation path. A cooperative viewer-to-viewer protocol is
insufficient, while normal simulator-wide animation visibility requires an
uploaded asset.

Second Life does not currently expose a viewer permission that lets agent A send
joint rotations through the simulator as agent B. A Firestorm implementation
therefore needs both a consent protocol and a transport for pose data. Merely
removing the current self-only check would pose the other avatar only in the
controller's local scene.

Paid per-pose animation publication is explicitly unacceptable for this
project. It is documented below only to explain the protocol boundary and is
not a proposed solution.

There is a no-cost option if "every viewer" means every consenting group member
running the modified Firestorm. The controller can fan out the authoritative
state of every avatar to every participant. All session viewers then render the
same group locally. Unmodified viewers and nonparticipants will not see it.

Second Life also has an experimental **Puppetry** protocol designed to stream
joint transforms through the simulator to nearby viewers. It would be the
correct foundation for live, universally relayed poser edits. However, the
official documentation still describes it as experimental, limited to
Puppetry-enabled viewers and regions, and not deployed everywhere. This
Firestorm tree contains the reserved message blocks but none of the Puppetry
capability, packing, decoding, buffering, or motion implementation.

This was also checked directly against the local upstream snapshot at
`.phoenix-firestorm-master` (`master` at
`b6f9d91d5e31427ee93e21937ac4c823b1eb7e61`). It has the same empty outgoing
`PhysicalAvatarEventList` blocks and no Puppetry capability or implementation.

## Evidence in the current code

The poser already contains an apparent extension point:

- `FSFloaterPoser::onPoseStartStop()` checks `havePermissionToAnimateAvatar()`
  before starting.
- `havePermissionToAnimateAvatar()` permits the self avatar and owned control
  avatars only.
- `havePermissionToAnimateOtherAvatar()` exists but unconditionally returns
  false.

See `indra/newview/fsfloaterposer.cpp:1451-1514`.

Posing is local viewer state. `FSPoserAnimator::tryPosingAvatar()` creates or
finds a synthetic `FSPosingMotion`, registers it on the local `LLVOAvatar`, and
calls `avatar->startMotion()`. It does not call
`gAgent.sendAnimationRequest()`. The synthetic motion UUID is generated from a
local transaction and is not uploaded as an animation asset. See
`indra/newview/fsposeranimator.cpp:1476-1571`.

`FSPosingMotion::onUpdate()` directly interpolates local joint-state rotation,
position, and scale values. See `indra/newview/fsposingmotion.cpp:32-119`.

The normal network animation path carries animation asset UUIDs, not joint
transforms:

- `AgentAnimation` is viewer-to-simulator and includes the authenticated
  `AgentID` and `SessionID`, followed by animation UUID/start flags.
- `AvatarAnimation` is simulator-to-viewer and likewise distributes animation
  UUIDs and sequence IDs, not arbitrary joint rotations.

Both messages also contain an optional `PhysicalAvatarEventList`. The current
tree always sends that block empty. Experimental Second Life Puppetry defines a
packed joint position/orientation stream in this block and has the simulator
relay it to eligible viewers.

See `scripts/messages/message_template.msg:1698-1716` and
`scripts/messages/message_template.msg:3593-3613`. The sender implementation in
`indra/newview/llagent.cpp:3882-3942` always uses the logged-in agent's identity
and session. A controller cannot legitimately substitute the target avatar's
identity.

## Rejected static group-pose architecture

This entire section is rejected because it requires paid animation uploads.

### 1. Edit locally

The controller requests explicit, session-scoped consent from every target and
poses the local representations of those avatars. During this stage, only the
controller is guaranteed to see the work.

### 2. Publish one animation per participant

On commit, convert each target's selected joint rotations into a looping static
animation and upload it through the normal animation asset pipeline. The viewer
must show the total upload charge and require explicit confirmation before any
paid uploads.

The resulting asset must be made available to the target. The conservative
workflow transfers an appropriately permissioned animation inventory item to
the target. A custom target viewer can then start that item after acceptance by
calling its own `gAgent.sendAnimationRequest()`.

An in-world controller object is another established route. It can contain the
uploaded animations and use scripts granted `PERMISSION_TRIGGER_ANIMATION` by
each participant. Because script animation permission belongs to one agent, a
group controller needs independent permission-holding script instances for the
participants.

### 3. Start on each target

Each target, not the controller, starts its assigned animation. Its
`AgentAnimation` message is authenticated with its own agent and session IDs.
The simulator then emits `AvatarAnimation` for that avatar, allowing ordinary
viewers in range to download and render the asset. A ready/commit handshake can
start the group together; exact timing is not important for a static pose.

Stopping or revoking the group session makes each target stop its animation.
Rotations are the appropriate initial scope. Poser scale and collision-volume
edits cannot be represented by the normal avatar animation broadcast path.

### 4. Canonical transforms and group alignment

Every viewer has its own interpolated `LLVOAvatar` state. Therefore the
controller must never send screen-frame rotations, world-frame deltas, or
"rotate from whatever you currently see" commands. Those reproduce the same
divergence seen when locally snapping avatar bodies in the compass feature.

The published pose must contain **absolute parent-local joint rotations**. Once
all viewers resolve the same animation asset UUID, they evaluate the same joint
values independently; no viewer's current rendered rotation is used as the
starting authority. Interactive preview messages should use the same absolute
parent-local representation and periodically resend complete state rather than
accumulating deltas.

Avatar world position and heading are separate from skeleton joint rotations.
An animation does not make each viewer's locally interpolated avatar-root state
authoritative. For a tightly aligned multi-avatar composition, use a shared
simulator-visible anchor, preferably a pose stand/linkset with one sit target
per participant. The simulator then replicates the seats' object transforms and
the seated avatar roots to all viewers. Each seat supplies a fixed position and
yaw; its assigned animation supplies the canonical local skeleton pose.

Without seats, each target viewer can consent to set its own agent position and
heading and send the normal agent update. Other viewers should converge after
the simulator relays it, but transient interpolation differences remain and it
is less deterministic for a group photograph. Directly changing remote
`LLVOAvatar::mRoot` transforms in the controller viewer affects that viewer
only and cannot solve group alignment.

## Bilateral live-preview architecture

For groups this becomes a session-wide fan-out, not a collection of unrelated
bilateral views. The controller is the sole pose authority and sends every
participant the state of every posed avatar. For `N` participants, each
accepted update identifies one avatar and is distributed to the other `N-1`
viewers; clients must not derive that avatar's pose from their pre-existing
local rendering.

### 1. Explicit consent session

Add a remote-poser session manager in a new module rather than expanding the
floater or posing-motion classes substantially.

The controller sends a request containing a protocol version, controller UUID,
target UUID, requested capabilities, a random session nonce, and an expiry. The
target receives a notification naming the controller and chooses Allow or Deny.
Allow should be session-scoped by default, not permanent.

On acceptance, both viewers bind the grant to:

- the authenticated sender UUID supplied by the transport;
- both avatar UUIDs;
- the random session nonce;
- the granted operations, initially rotation only;
- an expiry and monotonically increasing sequence number.

The target must have an always-available Stop control. Revoke on logout,
teleport/region change if same-region operation is retained, controller loss,
timeout, or explicit stop. Deny requests from muted or blocked users and rate
limit requests before showing notifications.

### 2. Synchronize pose state

After acceptance, the controller can continue using the existing poser on its
local representation of the target. Send only changed joints, with periodic
complete snapshots for recovery. Each update should carry stable joint
identifiers, local-space quaternions, sequence number, and session nonce.

The target viewer validates the session and applies the update through an
`FSPosingMotion` attached to `gAgentAvatarp`. It should not accept an arbitrary
target UUID in an update: a recipient may apply remote updates only to its own
avatar. The controller also retains its local posing motion, so both endpoints
show the same result.

In a group session, recipients also apply authorized state for the other remote
participants. A complete snapshot contains all participants' absolute
parent-local joint rotations plus canonical root position and heading. The
controller sequences snapshots globally; recipients discard older revisions.
This avoids compass-like divergence caused by viewer-local roots or accumulated
deltas. Each participant's own viewer remains the authority for consent and can
stop applying the session instantly.

Rotation-only scope is the safest first upstream proposal. Position and scale
change appearance locally and introduce more compatibility, abuse, and reset
semantics. Collision-volume control should remain excluded initially.

### 3. Transport choices

A low-volume consent handshake can plausibly use a recognizable, versioned
P2P-IM control envelope that supporting viewers consume before normal IM UI
display. The receiving code must trust the transport's `from_id`, never a UUID
inside the payload.

P2P IM is a poor transport for unthrottled drag updates: messages are limited,
server-routed, may be stored offline, interact with mute/DND behavior, and can
spam or throttle. It is reasonable for a first proof of concept that sends a
complete pose on commit or heavily coalesced changes. Smooth live manipulation
needs a supported higher-rate relay or simulator protocol; Firestorm cannot add
that unilaterally without either external infrastructure or Linden Lab support.

`GenericMessage` is not automatically a viewer-to-viewer tunnel. It is a
viewer/simulator message whose method must be handled and routed by server-side
code, so inventing a method name in Firestorm alone does not solve transport.

## Visibility limits

There are three distinct outcomes:

1. **Controller only:** current local posing of a remote `LLVOAvatar`; no remote
   protocol is needed, but the target does not see it.
2. **Controller and consenting target:** feasible with the cooperative live
   preview protocol above. Both viewers independently run equivalent local
   posing motions.
3. **Everybody nearby, static pose:** feasible after uploading one animation per
   participant and having each participant or an authorized script start it.
4. **Everybody nearby, live edits:** architecturally supported by experimental
   Puppetry, but not available in this Firestorm tree or generally deployable on
   the main grid according to the current official documentation.

For static publication, the poser can already export BVH data. Uploaded
animations are visible in-world, while locally played animations are not.
Script `PERMISSION_TRIGGER_ANIMATION` grants the script permission to start an
existing animation asset; it does not grant another viewer an arbitrary live
joint stream. Re-uploading assets during manipulation would be slow, costly,
and unsuitable for interactive use.

## Puppetry as the long-term live solution

The official Puppetry design is nearly an exact match for live group posing:

- a transmitting viewer sends packed joint positions and orientations in
  `PhysicalAvatarEventList` blocks of `AgentAnimation`;
- the simulator relays recent blocks in `AvatarAnimation` interest-list
  updates;
- receiving viewers decode and apply them through an IK/motion layer;
- data is range-limited by default, with explicit subscriptions available;
- the stream is deliberately lossy and drops old events under load.

The permission handoff would still be viewer-to-viewer: the controller sends
pose deltas to each consenting target, then each target's viewer republishes its
own transforms through Puppetry. This preserves simulator authentication: no
viewer impersonates another agent.

Puppetry does not mean literally every legacy viewer. Only viewers implementing
and enabling Puppetry can render the streamed transforms. Therefore uploaded
static animations remain the only compatible route for all ordinary viewers.

Official references:

- [Puppetry Network Control](https://wiki.secondlife.com/wiki/Puppetry_Network_Control)
- [How Puppetry Works](https://wiki.secondlife.com/wiki/How_Puppetry_Works)
- [Puppetry Development](https://wiki.secondlife.com/wiki/Puppetry_Development)
- [Internal Animation Format](https://wiki.secondlife.com/wiki/Internal_Animation_Format)
- [`llStartAnimation`](https://wiki.secondlife.com/wiki/LlStartAnimation)

## Upstream viability

The consent UI and authorization model are straightforward and align with the
existing dormant `havePermissionToAnimateOtherAvatar()` boundary. For a static
group workflow, the main complications are paid asset upload, inventory
delivery, cleanup, and ensuring every target starts/stops the correct asset. For
live universal visibility, the blocker is deployment and adoption of a
simulator-relayed joint protocol such as Puppetry.

If Linden Lab deploys Puppetry or a successor broadly, the most reviewable
Firestorm progression would be:

1. rotations only;
2. same-region avatars only;
3. explicit one-session consent and immediate revoke;
4. controller-to-target absolute parent-local pose updates;
5. target-side transmission through the simulator joint-stream capability;
6. immediate revoke and target-side stop controls;
7. protocol documented and versioned so unsupported viewers safely ignore it.

Until that server capability is broadly available, Firestorm can provide only
a consenting-viewers preview; it cannot make arbitrary unpaid poser output
visible to ordinary third-party viewers. Smooth in-world live updates should be
proposed as a Puppetry revival or successor, not built on high-rate hidden IM
traffic or hidden/temporary asset uploads.
