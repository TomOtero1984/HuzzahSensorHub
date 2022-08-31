# Description
Notes for using the ESP idf.py tool on Windows

### Install	
	1. CMake, `choco install cmake`
	2. Ninja, `choco install ninja`

### IDF_PATH
	Define a environment variable for 
	Example: C:\Espressif\frameworks\esp-idf-v4.4.2

### python_env
	Start the python_env by using the activate script
	`C:\Espressif\python_env\idf4.4_py3.10_env\Scripts\activate.ps1`

### build tool
	Use the build tool with the following command:
	`python $env:IDF_PATH/tools/idf.py`