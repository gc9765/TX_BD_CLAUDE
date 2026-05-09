.PHONY: clean All Project_Title Project_PreBuild Project_Build Project_PostBuild

All: Project_Title Project_PreBuild Project_Build Project_PostBuild

Project_Title:
	@echo "----------Building project:[ fpv_app_umac4 - FLASH ]----------"

Project_PreBuild:
	@echo Executing Pre Build commands ...
	@export CDKPath="D:/C-Sky/CDK" CDK_VERSION="V2.24.14" CPU="CK803S" ProjectName="fpv_app_umac4" ProjectPath="D:/Work/Customer_Engineering/Childrean_printer/Interaction1/TXW81x_FPV/project/" && D:/Work/Customer_Engineering/Childrean_printer/Interaction1/TXW81x_FPV/project/prebuild.sh $<
	@echo Done

Project_Build:
	@make -r -f fpv_app_umac4.mk -j 8 -C  ./ 

Project_PostBuild:
	@echo Executing Post Build commands ...
	@export CDKPath="D:/C-Sky/CDK" CDK_VERSION="V2.24.14" CPU="CK803S" ProjectName="fpv_app_umac4" ProjectPath="D:/Work/Customer_Engineering/Childrean_printer/Interaction1/TXW81x_FPV/project/" && D:/Work/Customer_Engineering/Childrean_printer/Interaction1/TXW81x_FPV/project/BuildBIN.sh
	@echo Done


clean:
	@echo "----------Cleaning project:[ fpv_app_umac4 - FLASH ]----------"

