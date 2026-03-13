#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class UFactory;
class UMaterialInstanceConstant;
class UMaterialInterface;
class UImportSubsystem;
class UObject;
class UStaticMesh;
class UTexture2D;
class UEditorAssetSubsystem;

enum class EManagedTextureKind : uint8
{
	Unknown,
	BaseColor,
	Normal,
	Orm
};

struct FManagedTextureInfo
{
	EManagedTextureKind Kind = EManagedTextureKind::Unknown;
	FString SourceFilename;
	FString SourceFolderName;
	FString MaterialGroupName;
	FString TargetFolderPath;
	FString TargetTextureAssetPath;
};

struct FManagedTextureGroup
{
	bool bHasBaseColor = false;
	bool bHasNormal = false;
	bool bHasOrm = false;
	TObjectPtr<UTexture2D> BaseColor = nullptr;
	TObjectPtr<UTexture2D> Normal = nullptr;
	TObjectPtr<UTexture2D> Orm = nullptr;

	bool IsComplete() const;
	FString DescribeMissing() const;
	int32 GetAvailableCount() const;
};

struct FPendingTextureOperation
{
	TWeakObjectPtr<UTexture2D> Texture;
	FManagedTextureInfo Info;
};

struct FManagedStaticMeshInfo
{
	FString SourceFilename;
	FString SourceFolderName;
	FString MeshAssetName;
	FString MaterialFolderPath;
	FString TargetMeshAssetPath;
};

struct FPendingStaticMeshOperation
{
	TWeakObjectPtr<UStaticMesh> StaticMesh;
	FManagedStaticMeshInfo Info;
};

struct FManagedFallbackMaterialInfo
{
	FString SourceFilename;
	FString SourceFolderName;
	FString TargetFolderPath;
	FString TargetAssetPath;
};

struct FPendingFallbackMaterialOperation
{
	TWeakObjectPtr<UMaterialInterface> Material;
	FManagedFallbackMaterialInfo Info;
};

class FOcrTextureToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterDelegates();
	void UnregisterDelegates();
	void HandleAssetPostImport(UFactory* InFactory, UObject* InCreatedObject);
	void HandleAssetReimport(UObject* InCreatedObject);
	void ProcessImportedObject(UObject* InObject, const TCHAR* InReason);
	void ProcessImportedTexture(UTexture2D* Texture, const TCHAR* InReason);
	void ProcessImportedStaticMesh(UStaticMesh* StaticMesh, const TCHAR* InReason);
	void ProcessImportedFallbackMaterial(UMaterialInterface* Material, const TCHAR* InReason);
	bool TryBuildManagedTextureInfo(UTexture2D* Texture, FManagedTextureInfo& OutInfo) const;
	bool TryBuildManagedStaticMeshInfo(UStaticMesh* StaticMesh, FManagedStaticMeshInfo& OutInfo) const;
	bool TryBuildManagedFallbackMaterialInfo(UMaterialInterface* Material, FManagedFallbackMaterialInfo& OutInfo) const;
	bool TryParseManagedTextureName(const FString& TextureName, EManagedTextureKind& OutKind, FString& OutGroupName) const;
	bool ApplyTextureRules(UTexture2D* Texture, EManagedTextureKind Kind) const;
	bool EnsureTargetFolder(const FString& FolderPath) const;
	bool MoveTextureToManagedFolder(UTexture2D* Texture, const FManagedTextureInfo& Info) const;
	bool MoveStaticMeshToManagedFolder(UStaticMesh* StaticMesh, const FManagedStaticMeshInfo& Info) const;
	bool MoveFallbackMaterialToManagedFolder(UMaterialInterface* Material, const FManagedFallbackMaterialInfo& Info) const;
	bool GatherManagedTextureGroups(const FString& TargetFolderPath, TMap<FString, FManagedTextureGroup>& OutGroups) const;
	UMaterialInterface* LoadParentMaterial() const;
	UMaterialInstanceConstant* FindOrCreateMaterialInstance(const FManagedTextureInfo& Info, UMaterialInterface* ParentMaterial, bool& bOutCreated) const;
	bool UpdateMaterialInstance(const FManagedTextureInfo& Info, const FManagedTextureGroup& Group) const;
	bool ApplyManagedMaterialInstances(UStaticMesh* StaticMesh, const FManagedStaticMeshInfo& Info) const;
	bool ArchiveImportedFallbackMaterials(UStaticMesh* StaticMesh, const FManagedStaticMeshInfo& Info) const;
	void NotifyUser(const FString& Message, bool bIsSuccess) const;
	void ReportConfigurationError(const FString& Key, const FString& Message) const;
	void ReportIncompleteGroup(const FManagedTextureInfo& Info, const FManagedTextureGroup& Group, const TCHAR* InReason);
	void ClearIncompleteReport(const FManagedTextureInfo& Info);
	UEditorAssetSubsystem* GetEditorAssetSubsystem() const;
	void QueuePendingTextureOperation(UTexture2D* Texture, const FManagedTextureInfo& Info);
	void QueuePendingStaticMeshOperation(UStaticMesh* StaticMesh, const FManagedStaticMeshInfo& Info);
	void QueuePendingFallbackMaterialOperation(UMaterialInterface* Material, const FManagedFallbackMaterialInfo& Info);
	bool FlushPendingImportOperations(float DeltaTime);
	bool ProcessPendingTextureOperation(const FPendingTextureOperation& PendingOperation);
	bool ProcessPendingStaticMeshOperation(const FPendingStaticMeshOperation& PendingOperation);
	bool ProcessPendingFallbackMaterialOperation(const FPendingFallbackMaterialOperation& PendingOperation);
	void ProcessManagedFolder(const FManagedTextureInfo& FolderInfo);

	FDelegateHandle PostImportHandle;
	FDelegateHandle ReimportHandle;
	FTSTicker::FDelegateHandle PendingImportTickerHandle;
	TWeakObjectPtr<UImportSubsystem> CachedImportSubsystem;
	mutable TSet<FString> ReportedConfigurationErrors;
	TSet<FString> ReportedIncompleteGroups;
	TMap<FString, FPendingTextureOperation> PendingTextureOperations;
	TMap<FString, FPendingStaticMeshOperation> PendingStaticMeshOperations;
	TMap<FString, FPendingFallbackMaterialOperation> PendingFallbackMaterialOperations;
	double PendingImportOperationDeadline = 0.0;
};
