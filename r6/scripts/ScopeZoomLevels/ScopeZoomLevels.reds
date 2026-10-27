// Scope Zoom Levels (prototype): a key cycles how much the scope lens magnifies.
// Each zoom level is an extra mesh appearance on the scope's lens part, named
// "<skin>_szl<level>". Level 0 is the vanilla appearance.
//
// A scope takes part through two TweakDB flats keyed by its model name (the item's
// appearanceResourceName), so every item, variant and skin copy of that scope shares them:
//   ScopeZoomLevels.<model>.lensParts: [ n"<lens mesh component>" ]
//   ScopeZoomLevels.<model>.zoomLevels: [ 1.3, 1.6, 2.0 ]   // levels 1, 2, 3 (magnification)
// Our own scopes are declared in r6\tweaks\ScopeZoomLevels\scopes.yaml.
module ScopeZoomLevels

// The lens parts of the fitted scope (some scopes have more than one, e.g. a lens cap)
// and the zoom levels the scope declares.
public class SZLLens {
  public let weapon: wref<ItemObject>;
  public let parts: array<ref<entSkinnedMeshComponent>>;
  public let levels: array<Float>;
}

// Runs the display re-attach again a moment after an appearance switch, once the
// renderer has caught up with the new appearance.
public class SZLRebindCallback extends DelayCallback {
  public let system: wref<SZLSystem>;
  public let weapon: wref<ItemObject>;
  public let parts: array<CName>;

  public func Call() -> Void {
    if IsDefined(this.system) { this.system.RebindDisplays(this.weapon, this.parts, false); }
  }
}

public class SZLSettings {
  @runtimeProperty("ModSettings.mod", "Scope Zoom Levels")
  @runtimeProperty("ModSettings.category", "Controls")
  @runtimeProperty("ModSettings.displayName", "Next lens zoom level")
  public let cycleKey: EInputKey = EInputKey.IK_Mouse5;

  @runtimeProperty("ModSettings.mod", "Scope Zoom Levels")
  @runtimeProperty("ModSettings.category", "Aiming")
  @runtimeProperty("ModSettings.displayName", "No camera zoom when aiming")
  @runtimeProperty("ModSettings.description", "Aiming keeps the normal field of view for every weapon; only scope lenses magnify.")
  public let noAimZoom: Bool = true;

  @runtimeProperty("ModSettings.mod", "Scope Zoom Levels")
  @runtimeProperty("ModSettings.category", "Debug")
  @runtimeProperty("ModSettings.displayName", "Debug logging")
  public let debugLog: Bool = false;
}

public class SZLSystem extends ScriptableSystem {
  private let m_settings: ref<SZLSettings>;
  private let m_level: Int32;
  private let m_upperBodyCallback: ref<CallbackHandle>;
  private let m_psm: wref<IBlackboard>;
  private let m_lastReported: ItemID; // so a miss is logged once per weapon, not every key press

  private func OnAttach() -> Void {
    this.m_settings = new SZLSettings();
    SZL_RegisterSettings(this.m_settings);
    GameInstance.GetCallbackSystem()
      .RegisterCallback(n"Input/Key", this, n"OnKeyInput")
      .SetLifetime(CallbackLifetime.Forever);
  }

  private func OnDetach() -> Void {
    SZL_UnregisterSettings(this.m_settings);
  }

  // A freshly equipped weapon spawns its scope with the vanilla appearance, so the
  // chosen level is put back every time the player starts aiming.
  private func OnPlayerAttach(request: ref<PlayerAttachRequest>) -> Void {
    let player: ref<PlayerPuppet> = request.owner as PlayerPuppet;
    if !IsDefined(player) { return; }
    let defs: ref<AllBlackboardDefinitions> = GetAllBlackboardDefs();
    let psm: ref<IBlackboard> = GameInstance.GetBlackboardSystem(this.GetGameInstance())
      .GetLocalInstanced(player.GetEntityID(), defs.PlayerStateMachine);
    if !IsDefined(psm) { return; }
    this.m_psm = psm;
    this.m_upperBodyCallback = psm.RegisterListenerInt(defs.PlayerStateMachine.UpperBody, this, n"OnUpperBodyChanged");
  }

  private func OnPlayerDetach(request: ref<PlayerDetachRequest>) -> Void {
    if IsDefined(this.m_psm) && IsDefined(this.m_upperBodyCallback) {
      this.m_psm.UnregisterListenerInt(GetAllBlackboardDefs().PlayerStateMachine.UpperBody, this.m_upperBodyCallback);
    }
    this.m_upperBodyCallback = null;
  }

  // The lens only magnifies while aiming: it enlarges what is drawn behind it on screen,
  // which is the scene when aiming but the gun itself when not. Not aiming, the lens goes
  // back to vanilla; the chosen level is kept for the next aim.
  protected cb func OnUpperBodyChanged(state: Int32) -> Bool {
    this.Apply(false);
    return true;
  }

  protected cb func OnKeyInput(event: ref<KeyInputEvent>) -> Void {
    if !Equals(event.GetAction(), EInputAction.IACT_Press) { return; }
    if !Equals(event.GetKey(), this.m_settings.cycleKey) { return; }
    if this.IsInMenu() || !this.IsAiming() { return; }
    this.Apply(true);
  }

  // Finds the lens of the fitted scope and gives it the current level (capped to the
  // levels that scope has), or vanilla when not aiming. With advance set, moves to the
  // next level first.
  private func Apply(advance: Bool) -> Void {
    let lens: ref<SZLLens> = this.FindLens(advance);
    if !IsDefined(lens) { return; }
    let count: Int32 = ArraySize(lens.levels) + 1; // level 0 is vanilla
    let level: Int32 = Min(this.m_level, count - 1);
    if advance {
      level = (level + 1) % count;
      this.m_level = level;
    }
    if !this.IsAiming() { level = 0; }
    let zoom: String = level > 0 ? FloatToStringPrec(lens.levels[level - 1], 2) + "x" : "vanilla";
    let switched: array<CName>;
    for part in lens.parts {
      let current: String = NameToString(part.meshAppearance);
      let skin: String = current;
      let cut: Int32 = StrFindFirst(current, "_szl");
      if cut >= 0 { skin = StrLeft(current, cut); }
      let target: String = skin;
      if level > 0 { target = skin + "_szl" + IntToString(level); }
      if !Equals(target, current) {
        let ok: Bool = part.ChangeAppearance(StringToName(target));
        this.Log("level " + IntToString(level) + " (" + zoom + "): " + NameToString(part.name) + " "
          + current + " -> " + target + (ok ? "" : " (refused)"));
        ArrayPush(switched, part.name);
      }
    }
    if ArraySize(switched) > 0 {
      this.RebindDisplays(lens.weapon, switched, true);
      let later: ref<SZLRebindCallback> = new SZLRebindCallback();
      later.system = this;
      later.weapon = lens.weapon;
      later.parts = switched;
      GameInstance.GetDelaySystem(this.GetGameInstance()).DelayCallback(later, 0.1, false);
    }
  }

  // Some scopes draw a display (e.g. the ClearVue's ammo counter) onto their lens mesh
  // through a UI component bound to that mesh part. Switching the part's appearance breaks
  // that binding, so the bound UI components are switched off and on to attach again.
  public func RebindDisplays(weapon: wref<ItemObject>, parts: array<CName>, report: Bool) -> Void {
    if !IsDefined(weapon) { return; }
    for c in weapon.GetComponents() {
      let display: ref<IWorldWidgetComponent> = c as IWorldWidgetComponent;
      if IsDefined(display) && IsDefined(display.meshTargetBinding)
          && ArrayContains(parts, display.meshTargetBinding.bindName) {
        display.Toggle(false);
        display.Toggle(true);
        if report { this.Log("display " + NameToString(display.name) + " re-attached"); }
      }
    }
  }

  // A fitted scope is not an entity of its own: its mesh components are built into the
  // weapon entity. The scope's settings name the lens part to look for there.
  private func FindLens(report: Bool) -> ref<SZLLens> {
    let game: GameInstance = this.GetGameInstance();
    let player: ref<PlayerPuppet> = GetPlayer(game);
    if !IsDefined(player) { return null; }
    let weapon: ref<ItemObject> = GameInstance.GetTransactionSystem(game)
      .GetItemInSlot(player, t"AttachmentSlots.WeaponRight");
    if !IsDefined(weapon) { return null; }

    let part: InnerItemData;
    weapon.GetItemData().GetItemPart(part, t"AttachmentSlots.Scope");
    let scopeID: TweakDBID = ItemID.GetTDBID(InnerItemData.GetItemID(part));
    if !TDBID.IsValid(scopeID) { return null; }
    let record: ref<Item_Record> = TweakDBInterface.GetItemRecord(scopeID);
    if !IsDefined(record) { return null; }
    let model: String = NameToString(record.AppearanceResourceName());

    let lens: ref<SZLLens> = new SZLLens();
    lens.weapon = weapon;
    lens.levels = TDB.GetFloatArray(TDBID.Create("ScopeZoomLevels." + model + ".zoomLevels"));
    let names: array<CName> = TDB.GetCNameArray(TDBID.Create("ScopeZoomLevels." + model + ".lensParts"));
    if ArraySize(lens.levels) > 0 {
      for name in names {
        let part: ref<entSkinnedMeshComponent> = weapon.FindComponentByName(name) as entSkinnedMeshComponent;
        if IsDefined(part) { ArrayPush(lens.parts, part); }
      }
      if ArraySize(lens.parts) > 0 { return lens; }
    }

    if report && ItemID.GetTDBID(weapon.GetItemID()) != ItemID.GetTDBID(this.m_lastReported) {
      this.m_lastReported = weapon.GetItemID();
      let line: String = "no lens zoom for scope " + TDBID.ToStringDEBUG(scopeID) + " (model " + model + ", "
        + IntToString(ArraySize(lens.levels)) + " levels, " + IntToString(ArraySize(names)) + " lens parts declared); weapon parts:";
      for c in weapon.GetComponents() {
        line += " " + NameToString(c.name);
      }
      this.Log(line);
    }
    return null;
  }

  private func IsAiming() -> Bool {
    if !IsDefined(this.m_psm) { return false; }
    return this.m_psm.GetInt(GetAllBlackboardDefs().PlayerStateMachine.UpperBody)
      == EnumInt(gamePSMUpperBodyStates.Aim);
  }

  // Not while a menu or photo mode is up.
  private func IsInMenu() -> Bool {
    let defs: ref<AllBlackboardDefinitions> = GetAllBlackboardDefs();
    let boards: ref<BlackboardSystem> = GameInstance.GetBlackboardSystem(this.GetGameInstance());
    let ui: ref<IBlackboard> = boards.Get(defs.UI_System);
    if IsDefined(ui) && ui.GetBool(defs.UI_System.IsInMenu) { return true; }
    let photo: ref<IBlackboard> = boards.Get(defs.PhotoMode);
    return IsDefined(photo) && photo.GetBool(defs.PhotoMode.IsActive);
  }

  private func Log(message: String) -> Void {
    if this.m_settings.debugLog { FTLog("[ScopeZoomLevels] " + message); }
  }

  public func NoAimZoom() -> Bool {
    return !IsDefined(this.m_settings) || this.m_settings.noAimZoom;
  }
}

func SZL_NoAimZoom(game: GameInstance) -> Bool {
  let system: ref<SZLSystem> = GameInstance.GetScriptableSystemsContainer(game)
    .Get(n"ScopeZoomLevels.SZLSystem") as SZLSystem;
  return IsDefined(system) && system.NoAimZoom();
}

// Weapon zoom (the ZoomLevel stat of the weapon plus its scope) reaches the camera
// through this animation data. After vanilla sends it, the same data is sent again
// with the weapon zoom at 1x, so the view keeps its field of view for every weapon.
// Kiroshi optics zoom (finalZoomLevel) is left alone.
@wrapMethod(ZoomTransition)
protected final func SendZoomAnimFeatureData(stateContext: ref<StateContext>, scriptInterface: ref<StateGameScriptInterface>) -> Void {
  wrappedMethod(stateContext, scriptInterface);
  let game: GameInstance = scriptInterface.owner.GetGame();
  if !SZL_NoAimZoom(game) { return; }
  let weapon: ref<WeaponObject> = GameInstance.GetTransactionSystem(game)
    .GetItemInSlot(scriptInterface.executionOwner, t"AttachmentSlots.WeaponRight") as WeaponObject;
  if !IsDefined(weapon) { return; }
  let stats: ref<StatsSystem> = scriptInterface.GetStatsSystem();
  let id: StatsObjectID = Cast<StatsObjectID>(weapon.GetEntityID());
  let data: ref<AnimFeature_Zoom> = new AnimFeature_Zoom();
  data.finalZoomLevel = this.GetCurrentZoomLevel(stateContext);
  data.weaponZoomLevel = 1.0;
  data.weaponScopeFov = stats.GetStatValue(id, gamedataStatType.ScopeFOV);
  data.weaponAimFOV = stats.GetStatValue(id, gamedataStatType.AimFOV);
  data.worldFOV = GameInstance.GetCameraSystem(game).GetActiveCameraFOV();
  data.zoomLevelNum = this.GetZoomLevelNumber(stateContext);
  data.noWeaponAimInTime = this.GetStaticFloatParameterDefault("noWeaponAimInTime", 0.20);
  data.noWeaponAimOutTime = this.GetStaticFloatParameterDefault("noWeaponAimOutTime", 0.20);
  data.shouldUseWeaponZoomStats = this.GetShouldUseWeaponZoomData(stateContext);
  data.focusModeActive = this.IsInVisionModeActiveState(stateContext, scriptInterface)
    || stateContext.IsStateActive(n"UpperBody", n"temporaryUnequip");
  scriptInterface.SetAnimationParameterFeature(n"ZoomAnimData", data);
}

@if(ModuleExists("ModSettingsModule"))
func SZL_RegisterSettings(settings: ref<SZLSettings>) -> Void {
  ModSettings.RegisterListenerToClass(settings);
}

@if(!ModuleExists("ModSettingsModule"))
func SZL_RegisterSettings(settings: ref<SZLSettings>) -> Void {}

@if(ModuleExists("ModSettingsModule"))
func SZL_UnregisterSettings(settings: ref<SZLSettings>) -> Void {
  ModSettings.UnregisterListenerToClass(settings);
}

@if(!ModuleExists("ModSettingsModule"))
func SZL_UnregisterSettings(settings: ref<SZLSettings>) -> Void {}
