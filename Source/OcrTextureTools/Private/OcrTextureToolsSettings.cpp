#include "OcrTextureToolsSettings.h"

UOcrTextureToolsSettings::UOcrTextureToolsSettings()
{
	bEnableAutomaticProcessing = false;
	WatchedSourceRoot.Path.Reset();
	TargetRootContentPath.Reset();
	bCaseSensitiveSuffixMatch = false;
	BaseColorSuffix = TEXT("_BaseColor");
	NormalSuffix = TEXT("_Normal");
	OrmSuffix = TEXT("_OcclusionRoughnessMetallic");
	ParentMaterialPath.Reset();
	BaseColorParameterName = TEXT("BaseColor");
	NormalParameterName = TEXT("Normal");
	OrmParameterName = TEXT("ORM");
	bShowEditorNotifications = true;
}

FName UOcrTextureToolsSettings::GetCategoryName() const
{
	return TEXT("Plugins");
}
