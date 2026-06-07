#include "MonolithMotionMatchingScaffoldActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithBlueprintComponentActions.h"
#include "MonolithBlueprintCDOActions.h"
#include "MonolithBlueprintCompileActions.h"
#include "MonolithBlueprintGraphActions.h"
#include "MonolithBlueprintNodeActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Animation/AnimBlueprint.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"

// Enhanced Input — UInputMappingContext / UInputAction live in the EnhancedInput
// module (verified: Engine/Plugins/EnhancedInput/Source/EnhancedInput/Public/).
#include "InputMappingContext.h"
#include "InputAction.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithMotionMatchingScaffoldActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_anim_class"),
		TEXT("Point a character/actor Blueprint's skeletal mesh component at an AnimBP's generated class. "
			 "Resolves the named component (on a Character BP the inherited mesh is 'Mesh'), sets its "
			 "AnimClass UPROPERTY to the AnimBP GeneratedClass, and warns (without failing) if the AnimBP "
			 "graph contains no Motion Matching node. Marks the Blueprint modified."),
		FMonolithActionHandler::CreateStatic(&HandleSetAnimClass),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("bp_path"), TEXT("Character/actor Blueprint asset path"))
			.Required(TEXT("component"), TEXT("string"), TEXT("Skeletal mesh component variable name (e.g. 'Mesh' on a Character BP)"))
			.RequiredAssetPath(TEXT("anim_bp_path"), TEXT("Animation Blueprint asset path whose generated class becomes the AnimClass"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("apply_movement_preset"),
		TEXT("Apply a CharacterMovementComponent locomotion preset to a character Blueprint's CDO. "
			 "Presets: 'orient_to_movement' (bOrientRotationToMovement=true, bUseControllerDesiredRotation=false, "
			 "RotationRate set) and 'strafe_controller_desired' (the inverse + MaxAcceleration set). "
			 "Marks the Blueprint modified."),
		FMonolithActionHandler::CreateStatic(&HandleApplyMovementPreset),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("bp_path"), TEXT("Character Blueprint asset path"))
			.Required(TEXT("preset"), TEXT("string"), TEXT("Preset name: 'orient_to_movement' | 'strafe_controller_desired'"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_engine_component_typed"),
		TEXT("Resolve a UActorComponent subclass by friendly name and add it to a Blueprint's construction "
			 "script. General utility. NOTE: CharacterTrajectory is intentionally NOT special-cased — there is "
			 "no CharacterTrajectoryComponent in UE 5.7; Motion Matching trajectory is AnimBP-side "
			 "(Pose History node bGenerateTrajectory)."),
		FMonolithActionHandler::CreateStatic(&HandleAddEngineComponentTyped),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("bp_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_type"), TEXT("string"), TEXT("UActorComponent subclass friendly name (e.g. 'SpringArmComponent')"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Variable name for the new component"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("scaffold_locomotion_input"),
		TEXT("Create a UInputMappingContext asset + one UInputAction asset per 'actions' entry, then add "
			 "event-graph nodes binding each action to AddMovementInput. Reuses add_event_node / add_nodes_bulk "
			 "/ connect_pins_bulk. Marks the Blueprint modified."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldLocomotionInput),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("bp_path"), TEXT("Character Blueprint asset path"))
			.RequiredAssetPath(TEXT("imc_path"), TEXT("Asset path for the new InputMappingContext"))
			.Required(TEXT("actions"), TEXT("array"), TEXT("Array of {name, value_type} — value_type one of Digital(bool)/Axis1D/Axis2D/Axis3D. One UInputAction asset created per entry."))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("validate_animbp_variable_contract"),
		TEXT("Reflection-walk the AnimBP's exposed (BlueprintReadWrite) variables against the character "
			 "Blueprint's published variables. Reports 'missing' (the ABP reads a var the BP does not publish) "
			 "and 'extra' (the BP publishes a var the ABP does not consume). Read-only — no mutation."),
		FMonolithActionHandler::CreateStatic(&HandleValidateAnimBpVariableContract),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("abp_path"), TEXT("Animation Blueprint asset path"))
			.RequiredAssetPath(TEXT("bp_path"), TEXT("Character Blueprint asset path"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("scaffold_motion_matching_character"),
		TEXT("COMPOSITE: create or reparent a character Blueprint (default parent ACharacter so it has Mesh + "
			 "CharacterMovementComponent), optionally set the skeletal mesh, then apply the AnimClass (set_anim_class) "
			 "and a CharacterMovementComponent preset (apply_movement_preset, default 'orient_to_movement'). "
			 "Trajectory is AnimBP-side (bGenerateTrajectory) — no trajectory component is added. Compiles the BP."),
		FMonolithActionHandler::CreateStatic(&HandleScaffoldMotionMatchingCharacter),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("bp_path"), TEXT("Character Blueprint asset path (created if absent, reparented if present)"))
			.Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent class for a newly-created BP (default 'Character')"), TEXT("Character"))
			.RequiredAssetPath(TEXT("anim_bp_path"), TEXT("Animation Blueprint asset path"))
			.OptionalAssetPath(TEXT("mesh"), TEXT("Skeletal mesh asset to assign to the mesh component (optional)"))
			.Optional(TEXT("movement_preset"), TEXT("string"), TEXT("CMC preset (default 'orient_to_movement')"), TEXT("orient_to_movement"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("get_inherited_component_override"),
		TEXT("READ-ONLY: report the effective value(s) of a component override on a child Blueprint. "
			 "Resolves the effective component template (CDO subobject for an inherited native component "
			 "like a Character's mesh, or the Inheritable Component Handler override for an SCS-inherited "
			 "component), reads the requested property (or a default set: AnimClass, SkeletalMesh, "
			 "AnimationMode) by reflection, and reports 'source' (cdo_native / ich / scs). This is the "
			 "verified read of what set_anim_class / set_mesh / set_component_property actually persisted."),
		FMonolithActionHandler::CreateStatic(&HandleGetInheritedComponentOverride),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("bp_path"), TEXT("Child Blueprint asset path"))
			.Required(TEXT("component"), TEXT("string"), TEXT("Component variable name or alias (e.g. 'Mesh' resolves to a Character's CharacterMesh0)"))
			.Optional(TEXT("property_name"), TEXT("string"), TEXT("Single property to read; if omitted, a default set is reported (AnimClass, SkeletalMesh, AnimationMode)"))
			.Build());
}

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------

namespace
{
	/** Build a sub-params object seeded with asset_path for delegation to existing handlers. */
	TSharedRef<FJsonObject> MakeSub(const FString& AssetPath)
	{
		TSharedRef<FJsonObject> Sub = MakeShared<FJsonObject>();
		Sub->SetStringField(TEXT("asset_path"), AssetPath);
		return Sub;
	}

	/**
	 * Shared component resolver: resolve a component on a Blueprint by alias, exact
	 * variable name, OR class — consistently for 5.1 / 5.2 / 5.3.
	 *
	 * Resolution order:
	 *   1. SCS-added node by exact (case-insensitive) variable name.
	 *   2. Native/inherited component on the CDO by exact name (e.g. "CharacterMesh0",
	 *      "CharMoveComp", or any author-named component).
	 *   3. Friendly alias (or empty name) → first component of the requested class on
	 *      the CDO. On a Character BP the inherited skeletal mesh is "CharacterMesh0"
	 *      (ACharacter::MeshComponentName) and the movement comp is "CharMoveComp"
	 *      (CharacterMovementComponentName) — so "Mesh"/"SkeletalMesh"/"CharacterMovement"
	 *      never match by name. We fall back to class match, which tolerates the engine's
	 *      private native component names without the caller knowing them.
	 *
	 * RequiredClass constrains every match; pass UActorComponent::StaticClass() for "any".
	 * Aliases is the set of friendly names that trigger the class fallback.
	 */
	UActorComponent* ResolveComponentOnBP(
		UBlueprint* BP,
		const FString& CompName,
		UClass* RequiredClass,
		const TArray<FString>& Aliases)
	{
		if (!BP || !RequiredClass) return nullptr;

		const bool bIsAlias = Aliases.ContainsByPredicate(
			[&CompName](const FString& A) { return A.Equals(CompName, ESearchCase::IgnoreCase); });

		// 1) SCS-added node by exact name (author-defined components).
		if (!CompName.IsEmpty() && BP->SimpleConstructionScript)
		{
			if (USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(FName(*CompName)))
			{
				if (Node->ComponentTemplate && Node->ComponentTemplate->IsA(RequiredClass))
				{
					return Node->ComponentTemplate;
				}
			}
		}

		if (!BP->GeneratedClass) return nullptr;
		UObject* CDO = BP->GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/false);
		AActor* CDOActor = Cast<AActor>(CDO);
		if (!CDOActor) return nullptr;

		TArray<UActorComponent*> Comps;
		CDOActor->GetComponents(Comps);

		// 2) Native/inherited component by exact name (e.g. "CharacterMesh0", custom name).
		if (!CompName.IsEmpty() && !bIsAlias)
		{
			for (UActorComponent* Comp : Comps)
			{
				if (!Comp || !Comp->IsA(RequiredClass)) continue;
				if (Comp->GetName().Equals(CompName, ESearchCase::IgnoreCase) ||
					Comp->GetFName() == FName(*CompName))
				{
					return Comp;
				}
			}
		}

		// 3) Alias OR empty name → first component of the required class on the CDO.
		//    Resolves "Mesh"/"SkeletalMesh" → CharacterMesh0 and the CMC lookup → CharMoveComp.
		if (bIsAlias || CompName.IsEmpty())
		{
			for (UActorComponent* Comp : Comps)
			{
				if (Comp && Comp->IsA(RequiredClass))
				{
					return Comp;
				}
			}
		}

		return nullptr;
	}

	/**
	 * Walk an AnimBP's graphs looking for a Motion Matching graph node by class name.
	 * Done by string match on the node class so MonolithBlueprint need not depend on
	 * the PoseSearchEditor module. Returns true if a UAnimGraphNode_MotionMatching*
	 * node is present.
	 */
	bool AnimBpHasMotionMatchingNode(UAnimBlueprint* ABP)
	{
		if (!ABP) return false;

		auto ScanGraphs = [](const TArray<TObjectPtr<UEdGraph>>& Graphs) -> bool
		{
			for (const TObjectPtr<UEdGraph>& Graph : Graphs)
			{
				if (!Graph) continue;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node || !Node->GetClass()) continue;
					const FString ClassName = Node->GetClass()->GetName();
					if (ClassName.Contains(TEXT("MotionMatching")))
					{
						return true;
					}
				}
			}
			return false;
		};

		if (ScanGraphs(ABP->FunctionGraphs)) return true;
		if (ScanGraphs(ABP->UbergraphPages)) return true;
		return false;
	}

	/** Collect the BlueprintReadWrite-exposed variable names of a generated class (declared on that class only). */
	void CollectExposedVarNames(UClass* GeneratedClass, TSet<FString>& Out)
	{
		if (!GeneratedClass) return;
		for (TFieldIterator<FProperty> It(GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop) continue;
			// BlueprintReadWrite => CPF_BlueprintVisible without CPF_BlueprintReadOnly.
			if (Prop->HasAnyPropertyFlags(CPF_BlueprintVisible) &&
				!Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
			{
				Out.Add(Prop->GetName());
			}
		}
	}
}

// ---------------------------------------------------------------------------
// 5.1 — set_anim_class
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithMotionMatchingScaffoldActions::HandleSetAnimClass(const TSharedPtr<FJsonObject>& Params)
{
	const FString BpPath = Params->GetStringField(TEXT("bp_path"));
	const FString CompName = Params->GetStringField(TEXT("component"));
	const FString AnimBpPath = Params->GetStringField(TEXT("anim_bp_path"));

	if (BpPath.IsEmpty())     return FMonolithActionResult::Error(TEXT("Missing required parameter: bp_path"));
	if (CompName.IsEmpty())   return FMonolithActionResult::Error(TEXT("Missing required parameter: component"));
	if (AnimBpPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: anim_bp_path"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(BpPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *BpPath));
	}

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AnimBpPath);
	if (!ABP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Animation Blueprint not found: %s"), *AnimBpPath));
	}
	if (!ABP->GeneratedClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Animation Blueprint '%s' has no GeneratedClass (compile it first)"), *AnimBpPath));
	}

	// Resolve by alias-or-name-or-class. "Mesh"/"SkeletalMesh" are friendly aliases that
	// fall back to the skeletal-mesh component on the CDO (ACharacter's is "CharacterMesh0",
	// NOT "Mesh"); an explicit exact name ("CharacterMesh0" or a custom name) still works.
	UActorComponent* MeshComp = ResolveComponentOnBP(
		BP, CompName, USkeletalMeshComponent::StaticClass(),
		{ TEXT("Mesh"), TEXT("SkeletalMesh") });
	USkeletalMeshComponent* SMC = Cast<USkeletalMeshComponent>(MeshComp);
	if (!SMC)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Skeletal mesh component '%s' not found on '%s' "
				 "(use 'Mesh'/'SkeletalMesh' alias, the exact name e.g. 'CharacterMesh0', or a custom name)"),
			*CompName, *BpPath));
	}

	// Warn (do not fail) if the AnimBP graph has no Motion Matching node.
	const bool bHasMMNode = AnimBpHasMotionMatchingNode(ABP);

	// Set the AnimClass UPROPERTY (TSubclassOf<UAnimInstance> AnimClass, SkeletalMeshComponent.h:372)
	// directly on the component template, then notify so it serialises. We use the public
	// SetAnimClass setter where the template is a live instance; for a template subobject
	// the direct field write + PostEditChange is the asset-time path mirrored by
	// set_component_property's setter discipline.
	//
	// PERSISTENCE: the mesh component is an INHERITED NATIVE component (ACharacter's
	// CharacterMesh0 has no SCS node, so no Inheritable Component Handler override exists).
	// The write lands on the CDO subobject directly. MarkBlueprintAsModified alone does NOT
	// re-serialise that CDO override — it silently reverts on the next reload/recompile.
	// The CDO override only persists if the Blueprint is structurally modified AND recompiled.
	BP->Modify();
	SMC->Modify();
	SMC->SetAnimInstanceClass(ABP->GeneratedClass);

	FProperty* AnimClassProp = USkeletalMeshComponent::StaticClass()->FindPropertyByName(TEXT("AnimClass"));
	if (AnimClassProp)
	{
		FPropertyChangedEvent ChangeEvent(AnimClassProp, EPropertyChangeType::ValueSet);
		SMC->PostEditChangeProperty(ChangeEvent);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FKismetEditorUtilities::CompileBlueprint(BP);
	BP->MarkPackageDirty();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("bp_path"), BpPath);
	Root->SetStringField(TEXT("component"), CompName);
	Root->SetStringField(TEXT("anim_bp_path"), AnimBpPath);
	Root->SetStringField(TEXT("anim_class"), ABP->GeneratedClass->GetName());
	Root->SetBoolField(TEXT("motion_matching_node_found"), bHasMMNode);
	if (!bHasMMNode)
	{
		Root->SetStringField(TEXT("warning"),
			TEXT("AnimBP graph contains no Motion Matching node — AnimClass was set anyway."));
	}
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// 5.2 — apply_movement_preset
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithMotionMatchingScaffoldActions::HandleApplyMovementPreset(const TSharedPtr<FJsonObject>& Params)
{
	const FString BpPath = Params->GetStringField(TEXT("bp_path"));
	const FString Preset = Params->GetStringField(TEXT("preset"));

	if (BpPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: bp_path"));
	if (Preset.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: preset"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(BpPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *BpPath));
	}

	// Resolve the CharacterMovementComponent BY CLASS on the CDO — the native inherited
	// CMC is named "CharMoveComp" (ACharacter::CharacterMovementComponentName), NOT
	// "CharacterMovement", so a name lookup fails. We resolve the UClass by name (body-only,
	// no CMC header needed) then find the component instance, and pass its REAL name to
	// set_component_property.
	UClass* CmcClass = FindFirstObject<UClass>(TEXT("CharacterMovementComponent"), EFindFirstObjectOptions::NativeFirst);
	if (!CmcClass)
	{
		CmcClass = FindFirstObject<UClass>(TEXT("UCharacterMovementComponent"), EFindFirstObjectOptions::NativeFirst);
	}
	if (!CmcClass)
	{
		return FMonolithActionResult::Error(TEXT("apply_movement_preset: UCharacterMovementComponent class not found"));
	}
	// Empty name → resolver returns the first component of the class on the CDO.
	UActorComponent* CmcComp = ResolveComponentOnBP(BP, FString(), CmcClass, {});
	if (!CmcComp)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("apply_movement_preset: no CharacterMovementComponent found on '%s' (is the parent a Character?)"), *BpPath));
	}
	const FString CmcName = CmcComp->GetName();

	// Build the property tree for this preset, then write each via set_component_property
	// (honours the Details-panel write path + PostEditChange notifications).
	TArray<TPair<FString, FString>> Writes;
	if (Preset.Equals(TEXT("orient_to_movement"), ESearchCase::IgnoreCase))
	{
		Writes.Emplace(TEXT("bOrientRotationToMovement"), TEXT("true"));
		Writes.Emplace(TEXT("bUseControllerDesiredRotation"), TEXT("false"));
		Writes.Emplace(TEXT("RotationRate"), TEXT("(Pitch=0.0,Yaw=500.0,Roll=0.0)"));
	}
	else if (Preset.Equals(TEXT("strafe_controller_desired"), ESearchCase::IgnoreCase))
	{
		Writes.Emplace(TEXT("bOrientRotationToMovement"), TEXT("false"));
		Writes.Emplace(TEXT("bUseControllerDesiredRotation"), TEXT("true"));
		Writes.Emplace(TEXT("MaxAcceleration"), TEXT("2048.0"));
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown preset '%s'. Expected 'orient_to_movement' or 'strafe_controller_desired'."), *Preset));
	}

	TArray<TSharedPtr<FJsonValue>> Applied;
	for (const TPair<FString, FString>& Write : Writes)
	{
		TSharedRef<FJsonObject> Sub = MakeSub(BpPath);
		Sub->SetStringField(TEXT("component_name"), CmcName);
		Sub->SetStringField(TEXT("property_name"), Write.Key);
		Sub->SetStringField(TEXT("value"), Write.Value);

		FMonolithActionResult R = FMonolithBlueprintComponentActions::HandleSetComponentProperty(Sub);
		if (!R.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("apply_movement_preset: failed to set '%s' on %s — %s"),
				*Write.Key, *CmcName, *R.ErrorMessage));
		}
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("property"), Write.Key);
		A->SetStringField(TEXT("value"), Write.Value);
		Applied.Add(MakeShared<FJsonValueObject>(A));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("bp_path"), BpPath);
	Root->SetStringField(TEXT("preset"), Preset);
	Root->SetStringField(TEXT("component"), CmcName);
	Root->SetArrayField(TEXT("applied"), Applied);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// 5.3 — add_engine_component_typed
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithMotionMatchingScaffoldActions::HandleAddEngineComponentTyped(const TSharedPtr<FJsonObject>& Params)
{
	const FString BpPath = Params->GetStringField(TEXT("bp_path"));
	const FString CompType = Params->GetStringField(TEXT("component_type"));
	const FString CompName = Params->GetStringField(TEXT("component_name"));

	if (BpPath.IsEmpty())   return FMonolithActionResult::Error(TEXT("Missing required parameter: bp_path"));
	if (CompType.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: component_type"));
	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: component_name"));

	// Resolve the class by friendly name — same NativeFirst probe the chooser fix uses.
	// Accept bare or U-prefixed name. NOTE: CharacterTrajectory is NOT special-cased —
	// no such component exists in UE 5.7 (Motion Matching trajectory is AnimBP-side).
	UClass* CompClass = FindFirstObject<UClass>(*CompType, EFindFirstObjectOptions::NativeFirst);
	if (!CompClass)
	{
		CompClass = FindFirstObject<UClass>(*(TEXT("U") + CompType), EFindFirstObjectOptions::NativeFirst);
	}
	if (!CompClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component class not found: %s"), *CompType));
	}
	if (!CompClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Class '%s' is not a UActorComponent subclass"), *CompType));
	}

	// Delegate to the shipped add_component handler (resolves the class itself, but we
	// validated above for a precise error message). Pass the canonical class name.
	TSharedRef<FJsonObject> Sub = MakeSub(BpPath);
	Sub->SetStringField(TEXT("component_class"), CompClass->GetName());
	Sub->SetStringField(TEXT("component_name"), CompName);

	return FMonolithBlueprintComponentActions::HandleAddComponent(Sub);
}

// ---------------------------------------------------------------------------
// 5.4 — scaffold_locomotion_input
// ---------------------------------------------------------------------------

namespace
{
	/** Map a friendly value_type string to EInputActionValueType. Defaults to Boolean. */
	EInputActionValueType ParseValueType(const FString& In)
	{
		if (In.Equals(TEXT("Axis1D"), ESearchCase::IgnoreCase) || In.Equals(TEXT("float"), ESearchCase::IgnoreCase))
			return EInputActionValueType::Axis1D;
		if (In.Equals(TEXT("Axis2D"), ESearchCase::IgnoreCase) || In.Equals(TEXT("Vector2D"), ESearchCase::IgnoreCase))
			return EInputActionValueType::Axis2D;
		if (In.Equals(TEXT("Axis3D"), ESearchCase::IgnoreCase) || In.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
			return EInputActionValueType::Axis3D;
		return EInputActionValueType::Boolean; // Digital
	}

	/** Create a UDataAsset-derived asset (IMC or IA) at a /Game path. Returns nullptr on collision/failure. */
	UObject* CreateInputAsset(UClass* AssetClass, const FString& PackagePath, FString& OutError)
	{
		int32 LastSlash = INDEX_NONE;
		if (!PackagePath.FindLastChar(TEXT('/'), LastSlash))
		{
			OutError = FString::Printf(TEXT("Invalid asset path (no '/'): %s"), *PackagePath);
			return nullptr;
		}
		const FString AssetName = PackagePath.Mid(LastSlash + 1);
		if (AssetName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Asset path must not end with '/': %s"), *PackagePath);
			return nullptr;
		}

		if (FMonolithAssetUtils::AssetExists(PackagePath))
		{
			OutError = FString::Printf(TEXT("Asset already exists: %s"), *PackagePath);
			return nullptr;
		}

		UPackage* Pkg = CreatePackage(*PackagePath);
		if (!Pkg)
		{
			OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackagePath);
			return nullptr;
		}

		UObject* NewAsset = NewObject<UObject>(Pkg, AssetClass, FName(*AssetName), RF_Public | RF_Standalone);
		if (!NewAsset)
		{
			OutError = FString::Printf(TEXT("NewObject failed for %s at %s"), *AssetClass->GetName(), *PackagePath);
			return nullptr;
		}
		Pkg->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(NewAsset);
		return NewAsset;
	}
}

FMonolithActionResult FMonolithMotionMatchingScaffoldActions::HandleScaffoldLocomotionInput(const TSharedPtr<FJsonObject>& Params)
{
	const FString BpPath = Params->GetStringField(TEXT("bp_path"));
	const FString ImcPath = Params->GetStringField(TEXT("imc_path"));

	if (BpPath.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing required parameter: bp_path"));
	if (ImcPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: imc_path"));

	const TArray<TSharedPtr<FJsonValue>>* ActionsArr = nullptr;
	if (!Params->TryGetArrayField(TEXT("actions"), ActionsArr) || !ActionsArr || ActionsArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("'actions' is required and must be a non-empty array of {name, value_type}"));
	}

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(BpPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *BpPath));
	}

	// --- Create the InputMappingContext asset ---
	FString CreateError;
	UObject* ImcObj = CreateInputAsset(UInputMappingContext::StaticClass(), ImcPath, CreateError);
	if (!ImcObj)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("scaffold_locomotion_input: %s"), *CreateError));
	}

	// IA assets get created next to the IMC, under <imc_dir>/IA_<Name>.
	int32 ImcSlash = INDEX_NONE;
	ImcPath.FindLastChar(TEXT('/'), ImcSlash);
	const FString ImcDir = (ImcSlash != INDEX_NONE) ? ImcPath.Left(ImcSlash) : TEXT("/Game");

	TArray<TSharedPtr<FJsonValue>> CreatedActions;
	TArray<FString> ActionNames;

	for (const TSharedPtr<FJsonValue>& Entry : *ActionsArr)
	{
		const TSharedPtr<FJsonObject> Obj = Entry.IsValid() ? Entry->AsObject() : nullptr;
		if (!Obj.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Each 'actions' entry must be an object {name, value_type}"));
		}
		FString ActName;
		Obj->TryGetStringField(TEXT("name"), ActName);
		if (ActName.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Each 'actions' entry requires a non-empty 'name'"));
		}
		FString ValueType;
		Obj->TryGetStringField(TEXT("value_type"), ValueType);

		const FString IaPath = FString::Printf(TEXT("%s/IA_%s"), *ImcDir, *ActName);
		FString IaError;
		UObject* IaObj = CreateInputAsset(UInputAction::StaticClass(), IaPath, IaError);
		if (!IaObj)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("scaffold_locomotion_input: failed creating InputAction '%s' — %s"), *ActName, *IaError));
		}
		if (UInputAction* IA = Cast<UInputAction>(IaObj))
		{
			IA->ValueType = ParseValueType(ValueType);
			IA->MarkPackageDirty();
		}

		ActionNames.Add(ActName);
		TSharedPtr<FJsonObject> AOut = MakeShared<FJsonObject>();
		AOut->SetStringField(TEXT("name"), ActName);
		AOut->SetStringField(TEXT("ia_path"), IaPath);
		AOut->SetStringField(TEXT("value_type"), ValueType.IsEmpty() ? TEXT("Digital") : ValueType);
		CreatedActions.Add(MakeShared<FJsonValueObject>(AOut));
	}

	// --- Event-graph wiring: a BeginPlay/Tick-anchored AddMovementInput scaffold. ---
	// We drop one AddMovementInput CallFunction node per action so the locomotion
	// movement-input plumbing exists; binding the EnhancedInput action events is left
	// to the AnimBP/character author (the IA assets are now in place to bind against).
	int32 NodesAdded = 0;
	for (const FString& ActName : ActionNames)
	{
		TSharedRef<FJsonObject> NodeSub = MakeSub(BpPath);
		NodeSub->SetStringField(TEXT("node_type"), TEXT("call_function"));
		NodeSub->SetStringField(TEXT("function_name"), TEXT("AddMovementInput"));
		NodeSub->SetStringField(TEXT("target_class"), TEXT("Pawn"));
		FMonolithActionResult NR = FMonolithBlueprintNodeActions::HandleAddNode(NodeSub);
		if (NR.bSuccess)
		{
			NodesAdded++;
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("bp_path"), BpPath);
	Root->SetStringField(TEXT("imc_path"), ImcPath);
	Root->SetArrayField(TEXT("actions"), CreatedActions);
	Root->SetNumberField(TEXT("add_movement_input_nodes"), NodesAdded);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// 5.5 — validate_animbp_variable_contract
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithMotionMatchingScaffoldActions::HandleValidateAnimBpVariableContract(const TSharedPtr<FJsonObject>& Params)
{
	const FString AbpPath = Params->GetStringField(TEXT("abp_path"));
	const FString BpPath = Params->GetStringField(TEXT("bp_path"));

	if (AbpPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: abp_path"));
	if (BpPath.IsEmpty())  return FMonolithActionResult::Error(TEXT("Missing required parameter: bp_path"));

	UAnimBlueprint* ABP = FMonolithAssetUtils::LoadAssetByPath<UAnimBlueprint>(AbpPath);
	if (!ABP || !ABP->GeneratedClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Animation Blueprint not found or not compiled: %s"), *AbpPath));
	}

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(BpPath);
	if (!BP || !BP->GeneratedClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Blueprint not found or not compiled: %s"), *BpPath));
	}

	TSet<FString> AbpVars;
	CollectExposedVarNames(ABP->GeneratedClass, AbpVars);

	TSet<FString> BpVars;
	CollectExposedVarNames(BP->GeneratedClass, BpVars);

	// missing: ABP exposes/reads a variable the BP does not publish.
	// extra:   BP publishes a variable the ABP does not consume.
	TArray<TSharedPtr<FJsonValue>> Missing;
	for (const FString& V : AbpVars)
	{
		if (!BpVars.Contains(V)) Missing.Add(MakeShared<FJsonValueString>(V));
	}
	TArray<TSharedPtr<FJsonValue>> Extra;
	for (const FString& V : BpVars)
	{
		if (!AbpVars.Contains(V)) Extra.Add(MakeShared<FJsonValueString>(V));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("abp_path"), AbpPath);
	Root->SetStringField(TEXT("bp_path"), BpPath);
	Root->SetNumberField(TEXT("abp_exposed_var_count"), AbpVars.Num());
	Root->SetNumberField(TEXT("bp_published_var_count"), BpVars.Num());
	Root->SetArrayField(TEXT("missing"), Missing);
	Root->SetArrayField(TEXT("extra"), Extra);
	Root->SetBoolField(TEXT("contract_satisfied"), Missing.Num() == 0);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// 5.6 — scaffold_motion_matching_character (COMPOSITE — composes 5.1 + 5.2)
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithMotionMatchingScaffoldActions::HandleScaffoldMotionMatchingCharacter(const TSharedPtr<FJsonObject>& Params)
{
	const FString BpPath = Params->GetStringField(TEXT("bp_path"));
	const FString AnimBpPath = Params->GetStringField(TEXT("anim_bp_path"));

	if (BpPath.IsEmpty())     return FMonolithActionResult::Error(TEXT("Missing required parameter: bp_path"));
	if (AnimBpPath.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: anim_bp_path"));

	FString ParentClass = Params->GetStringField(TEXT("parent_class"));
	if (ParentClass.IsEmpty()) ParentClass = TEXT("Character");

	FString MovementPreset = Params->GetStringField(TEXT("movement_preset"));
	if (MovementPreset.IsEmpty()) MovementPreset = TEXT("orient_to_movement");

	FString Mesh;
	Params->TryGetStringField(TEXT("mesh"), Mesh);

	TArray<TSharedPtr<FJsonValue>> Steps;
	auto NoteStep = [&Steps](const FString& Name, bool bOk, const FString& Detail)
	{
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("step"), Name);
		S->SetBoolField(TEXT("success"), bOk);
		if (!Detail.IsEmpty()) S->SetStringField(TEXT("detail"), Detail);
		Steps.Add(MakeShared<FJsonValueObject>(S));
	};

	// --- Create or reparent the character Blueprint ---
	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(BpPath);
	if (!BP)
	{
		// Create new BP at bp_path with the chosen parent (default Character — gives Mesh + CMC).
		TSharedRef<FJsonObject> CreateSub = MakeShared<FJsonObject>();
		CreateSub->SetStringField(TEXT("save_path"), BpPath);
		CreateSub->SetStringField(TEXT("parent_class"), ParentClass);
		FMonolithActionResult CR = FMonolithBlueprintCompileActions::HandleCreateBlueprint(CreateSub);
		if (!CR.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("scaffold_motion_matching_character: create_blueprint failed — %s"), *CR.ErrorMessage));
		}
		NoteStep(TEXT("create_blueprint"), true, ParentClass);
		BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(BpPath);
		if (!BP)
		{
			return FMonolithActionResult::Error(TEXT("scaffold_motion_matching_character: BP created but failed to reload"));
		}
	}
	else
	{
		// Existing BP — reparent if the requested parent differs.
		const FString CurrentParent = BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None");
		if (!CurrentParent.Equals(ParentClass, ESearchCase::IgnoreCase) &&
			!CurrentParent.Equals(FString(TEXT("A")) + ParentClass, ESearchCase::IgnoreCase))
		{
			TSharedRef<FJsonObject> ReparentSub = MakeSub(BpPath);
			ReparentSub->SetStringField(TEXT("new_parent_class"), ParentClass);
			FMonolithActionResult RR = FMonolithBlueprintGraphActions::HandleReparentBlueprint(ReparentSub);
			NoteStep(TEXT("reparent_blueprint"), RR.bSuccess, RR.bSuccess ? ParentClass : RR.ErrorMessage);
		}
		else
		{
			NoteStep(TEXT("reparent_blueprint"), true, TEXT("already correct parent — skipped"));
		}
	}

	// --- Optional: set the skeletal mesh — resolve the mesh component BY CLASS, then write
	//     the mesh asset DIRECTLY via SetSkeletalMeshAsset + the full persistence handshake.
	//     The mesh component on a Character is an INHERITED NATIVE component (CharacterMesh0,
	//     no SCS node), so the write lands on the CDO subobject. As with set_anim_class, that
	//     override only persists if the Blueprint is structurally modified AND recompiled —
	//     MarkBlueprintAsModified alone reverts it on reload. ---
	if (!Mesh.IsEmpty())
	{
		UActorComponent* MeshComp = ResolveComponentOnBP(
			BP, TEXT("Mesh"), USkeletalMeshComponent::StaticClass(),
			{ TEXT("Mesh"), TEXT("SkeletalMesh") });
		USkeletalMeshComponent* MeshSMC = Cast<USkeletalMeshComponent>(MeshComp);
		USkeletalMesh* MeshAsset = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(Mesh);
		if (!MeshSMC)
		{
			NoteStep(TEXT("set_mesh"), false, TEXT("no skeletal mesh component found on BP"));
		}
		else if (!MeshAsset)
		{
			NoteStep(TEXT("set_mesh"), false, FString::Printf(TEXT("skeletal mesh asset not found: %s"), *Mesh));
		}
		else
		{
			BP->Modify();
			MeshSMC->Modify();
			MeshSMC->SetSkeletalMeshAsset(MeshAsset);

			// The persisted UPROPERTY is SkinnedAsset (SkeletalMeshAsset is Transient); notify
			// the serialised property so the override is recorded against the CDO subobject.
			if (FProperty* MeshProp = USkeletalMeshComponent::StaticClass()->FindPropertyByName(TEXT("SkinnedAsset")))
			{
				FPropertyChangedEvent ChangeEvent(MeshProp, EPropertyChangeType::ValueSet);
				MeshSMC->PostEditChangeProperty(ChangeEvent);
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
			FKismetEditorUtilities::CompileBlueprint(BP);
			BP->MarkPackageDirty();
			NoteStep(TEXT("set_mesh"), true, Mesh);
		}
	}

	// --- Compose 5.1: set_anim_class on the 'Mesh' component ---
	{
		TSharedRef<FJsonObject> AnimSub = MakeShared<FJsonObject>();
		AnimSub->SetStringField(TEXT("bp_path"), BpPath);
		AnimSub->SetStringField(TEXT("component"), TEXT("Mesh"));
		AnimSub->SetStringField(TEXT("anim_bp_path"), AnimBpPath);
		FMonolithActionResult AR = HandleSetAnimClass(AnimSub);
		if (!AR.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("scaffold_motion_matching_character: set_anim_class failed — %s"), *AR.ErrorMessage));
		}
		NoteStep(TEXT("set_anim_class"), true, AnimBpPath);
	}

	// --- Compose 5.2: apply_movement_preset ---
	{
		TSharedRef<FJsonObject> PresetSub = MakeShared<FJsonObject>();
		PresetSub->SetStringField(TEXT("bp_path"), BpPath);
		PresetSub->SetStringField(TEXT("preset"), MovementPreset);
		FMonolithActionResult PR = HandleApplyMovementPreset(PresetSub);
		if (!PR.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("scaffold_motion_matching_character: apply_movement_preset failed — %s"), *PR.ErrorMessage));
		}
		NoteStep(TEXT("apply_movement_preset"), true, MovementPreset);
	}

	// --- Compile ---
	{
		TSharedRef<FJsonObject> CompileSub = MakeSub(BpPath);
		FMonolithActionResult CR = FMonolithBlueprintCompileActions::HandleCompileBlueprint(CompileSub);
		NoteStep(TEXT("compile"), CR.bSuccess, CR.bSuccess ? TEXT("") : CR.ErrorMessage);
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("bp_path"), BpPath);
	Root->SetStringField(TEXT("parent_class"), ParentClass);
	Root->SetStringField(TEXT("anim_bp_path"), AnimBpPath);
	Root->SetStringField(TEXT("movement_preset"), MovementPreset);
	if (!Mesh.IsEmpty()) Root->SetStringField(TEXT("mesh"), Mesh);
	Root->SetArrayField(TEXT("steps"), Steps);
	Root->SetBoolField(TEXT("success"), true);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// 5.7 — get_inherited_component_override (READ-ONLY)
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithMotionMatchingScaffoldActions::HandleGetInheritedComponentOverride(const TSharedPtr<FJsonObject>& Params)
{
	const FString BpPath = Params->GetStringField(TEXT("bp_path"));
	const FString CompName = Params->GetStringField(TEXT("component"));
	FString SingleProp;
	Params->TryGetStringField(TEXT("property_name"), SingleProp);

	if (BpPath.IsEmpty())   return FMonolithActionResult::Error(TEXT("Missing required parameter: bp_path"));
	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("Missing required parameter: component"));

	UBlueprint* BP = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(BpPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *BpPath));
	}

	// Resolve the EFFECTIVE component template. ResolveComponentOnBP returns the SCS
	// template when the component is author-declared on this BP, otherwise the component
	// on the CDO (which reflects native defaults + any ICH override + inherited values).
	UActorComponent* Comp = ResolveComponentOnBP(
		BP, CompName, UActorComponent::StaticClass(),
		{ TEXT("Mesh"), TEXT("SkeletalMesh") });
	if (!Comp)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Component '%s' not found on '%s'"), *CompName, *BpPath));
	}

	// Classify the override source.
	//   scs        — declared as an SCS node on THIS Blueprint.
	//   ich        — SCS-inherited from a parent BP with an Inheritable Component Handler override.
	//   cdo_native — inherited native component (no SCS node anywhere); value read off the CDO.
	FString SourceClass = TEXT("cdo_native");
	{
		bool bIsThisBpScs = false;
		if (BP->SimpleConstructionScript)
		{
			for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && (Node->GetVariableName() == Comp->GetFName() ||
					(Node->ComponentTemplate && Node->ComponentTemplate->GetName().Equals(Comp->GetName(), ESearchCase::IgnoreCase))))
				{
					bIsThisBpScs = true;
					break;
				}
			}
		}

		if (bIsThisBpScs)
		{
			SourceClass = TEXT("scs");
		}
		else if (UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass))
		{
			// An SCS-inherited component (declared on a parent BP) carries its override on
			// this BP's Inheritable Component Handler. If an ICH exists and holds an override
			// template matching this component's name, classify as 'ich'; otherwise the value
			// is the plain inherited/native default read off the CDO.
			if (UInheritableComponentHandler* ICH = BPGC->GetInheritableComponentHandler(/*bCreateIfNecessary=*/false))
			{
				for (auto RecordIt = ICH->CreateRecordIterator(); RecordIt; ++RecordIt)
				{
					if (RecordIt->ComponentTemplate &&
						RecordIt->ComponentTemplate->GetName().Equals(Comp->GetName(), ESearchCase::IgnoreCase))
					{
						SourceClass = TEXT("ich");
						break;
					}
				}
			}
		}
	}

	// Build the property list to read.
	TArray<FString> PropsToRead;
	if (!SingleProp.IsEmpty())
	{
		PropsToRead.Add(SingleProp);
	}
	else
	{
		PropsToRead.Add(TEXT("AnimClass"));
		PropsToRead.Add(TEXT("SkeletalMesh"));
		PropsToRead.Add(TEXT("AnimationMode"));
	}

	TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
	for (const FString& PName : PropsToRead)
	{
		FProperty* Prop = Comp->GetClass()->FindPropertyByName(FName(*PName));
		if (!Prop)
		{
			for (TFieldIterator<FProperty> It(Comp->GetClass()); It; ++It)
			{
				if (It->GetName().Equals(PName, ESearchCase::IgnoreCase)) { Prop = *It; break; }
			}
		}
		if (!Prop)
		{
			// Property not present on this component class — report as not-applicable.
			PropsObj->SetStringField(PName, TEXT("<property not found on component>"));
			continue;
		}
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Comp);
		FString Exported;
		Prop->ExportText_Direct(Exported, ValuePtr, ValuePtr, const_cast<UActorComponent*>(Comp), PPF_None);
		PropsObj->SetStringField(Prop->GetName(), Exported);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("bp_path"), BpPath);
	Root->SetStringField(TEXT("component"), CompName);
	Root->SetStringField(TEXT("resolved_component"), Comp->GetName());
	Root->SetStringField(TEXT("component_class"), Comp->GetClass()->GetName());
	Root->SetStringField(TEXT("source"), SourceClass);
	Root->SetObjectField(TEXT("properties"), PropsObj);
	return FMonolithActionResult::Success(Root);
}
