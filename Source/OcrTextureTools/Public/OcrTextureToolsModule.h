#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class UFactory;
class UMaterialInstanceConstant;
class UMaterialInterface;
class UImportSubsystem;
class UObject;
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
	bool TryBuildManagedTextureInfo(UTexture2D* Texture, FManagedTextureInfo& OutInfo) const;
	bool TryParseManagedTextureName(const FString& TextureName, EManagedTextureKind& OutKind, FString& OutGroupName) const;
	bool ApplyTextureRules(UTexture2D* Texture, EManagedTextureKind Kind) const;
	bool EnsureTargetFolder(const FString& FolderPath) const;
	bool MoveTextureToManagedFolder(UTexture2D* Texture, const FManagedTextureInfo& Info) const;
	bool GatherManagedTextureGroups(const FString& TargetFolderPath, TMap<FString, FManagedTextureGroup>& OutGroups) const;
	UMaterialInterface* LoadParentMaterial() const;
	UMaterialInstanceConstant* FindOrCreateMaterialInstance(const FManagedTextureInfo& Info, UMaterialInterface* ParentMaterial, bool& bOutCreated) const;
	bool UpdateMaterialInstance(const FManagedTextureInfo& Info, const FManagedTextureGroup& Group) const;
	void NotifyUser(const FString& Message, bool bIsSuccess) const;
	void ReportIncompleteGroup(const FManagedTextureInfo& Info, const FManagedTextureGroup& Group, const TCHAR* InReason);
	void ClearIncompleteReport(const FManagedTextureInfo& Info);
	UEditorAssetSubsystem* GetEditorAssetSubsystem() const;
	void QueuePendingTextureOperation(UTexture2D* Texture, const FManagedTextureInfo& Info);
	bool FlushPendingTextureOperations(float DeltaTime);
	bool ProcessPendingTextureOperation(const FPendingTextureOperation& PendingOperation);
	void ProcessManagedFolder(const FManagedTextureInfo& FolderInfo);

	FDelegateHandle PostImportHandle;
	FDelegateHandle ReimportHandle;
	FTSTicker::FDelegateHandle PendingFolderTickerHandle;
	TWeakObjectPtr<UImportSubsystem> CachedImportSubsystem;
	TSet<FString> ReportedIncompleteGroups;
	TMap<FString, FPendingTextureOperation> PendingTextureOperations;
	double PendingTextureOperationDeadline = 0.0;
};
