.PHONY: clean All Project_Title Project_PreBuild Project_Build Project_PostBuild

All: Project_Title Project_PreBuild Project_Build Project_PostBuild

Project_Title:
	@echo "----------Building project:[ fpv_app_umac4 - FLASH ]----------"

Project_PreBuild:
	@echo Executing Pre Build commands ...
	@export CDKPath="D:/taixin/tx" CDK_VERSION="V2.8.8" ProjectPath="D:/taixin/Camera/TXW81x_FPV-v2.5.3.7-33277_AIbaidu/project/" && D:/taixin/Camera/TXW81x_FPV-v2.5.3.7-33277_AIbaidu/project/prebuild.sh $<
	@echo Done

Project_Build:
	@make -r -f fpv_app_umac4.mk -j 4 -C  ./ 

Project_PostBuild:
	@echo Executing Post Build commands ...
	@export CDKPath="D:/taixin/tx" CDK_VERSION="V2.8.8" ProjectPath="D:/taixin/Camera/TXW81x_FPV-v2.5.3.7-33277_AIbaidu/project/" && D:/taixin/Camera/TXW81x_FPV-v2.5.3.7-33277_AIbaidu/project/BuildBIN.sh
	@echo Done


clean:
	@echo "----------Cleaning project:[ fpv_app_umac4 - FLASH ]----------"

