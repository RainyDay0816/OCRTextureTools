#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "OcrTextureToolsSettings.generated.h"

UCLASS(Config=Editor, DefaultConfig, Meta=(DisplayName="OCR Texture Tools"))
class OCRTEXTURETOOLS_API UOcrTextureToolsSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UOcrTextureToolsSettings();

	virtual FName GetCategoryName() const override;

	UPROPERTY(Config, EditAnywhere, Category="OCR Rules", Meta=(ClampMin="1"))
	bool bEnableAutomaticProcessing;

	UPROPERTY(Config, EditAnywhere, Category="Import Scope")
	FDirectoryPath WatchedSourceRoot;

	UPROPERTY(Config, EditAnywhere, Category="Import Scope")
	FString TargetRootContentPath;

	UPROPERTY(Config, EditAnywhere, Category="Import Scope")
	FString StaticMeshTargetRootContentPath;

	UPROPERTY(Config, EditAnywhere, Category="Suffix Rules")
	bool bCaseSensitiveSuffixMatch;

	UPROPERTY(Config, EditAnywhere, Category="Suffix Rules")
	FString BaseColorSuffix;

	UPROPERTY(Config, EditAnywhere, Category="Suffix Rules")
	FString NormalSuffix;

	UPROPERTY(Config, EditAnywhere, Category="Suffix Rules")
	FString OrmSuffix;

	UPROPERTY(Config, EditAnywhere, Category="Material Setup", Meta=(AllowedClasses="/Script/Engine.MaterialInterface"))
	FSoftObjectPath ParentMaterialPath;

	UPROPERTY(Config, EditAnywhere, Category="Material Setup")
	FName BaseColorParameterName;

	UPROPERTY(Config, EditAnywhere, Category="Material Setup")
	FName NormalParameterName;

	UPROPERTY(Config, EditAnywhere, Category="Material Setup")
	FName OrmParameterName;

	UPROPERTY(Config, EditAnywhere, Category="Feedback")
	bool bShowEditorNotifications;
};
