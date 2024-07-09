#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <iostream>
#include <vector>

void print_pyobject(PyObject* pResult) {
	if (pResult == NULL) {
		PyErr_Print();
		return;
	}
	if (PyUnicode_Check(pResult)) {

		std::cout << "Function returned: " << PyUnicode_AsUTF8String(pResult) << std::endl;
	}
	else if (PyLong_Check(pResult)) {
		std::cout << "Function returned: " << PyLong_AsLong(pResult) << std::endl;
	}
	else if (PyFloat_Check(pResult)) {
		std::cout << "Function returned: " << PyFloat_AsDouble(pResult) << std::endl;
	}
	else if (PyList_Check(pResult)) {
		std::cout << "Function returned a list:" << std::endl;
		Py_ssize_t size = PyList_Size(pResult);
		for (Py_ssize_t i = 0; i < size; ++i) {
			PyObject* item = PyList_GetItem(pResult, i);
			print_pyobject(item);
		}
	}
	else if (PyTuple_Check(pResult)) {
		std::cout << "Function returned a tuple:" << std::endl;
		Py_ssize_t size = PyTuple_Size(pResult);
		for (Py_ssize_t i = 0; i < size; ++i) {
			PyObject* item = PyTuple_GetItem(pResult, i);
			print_pyobject(item);
			std::cout << i << " in tuple  = " << PyFloat_AsDouble(item) << std::endl;
		}
	}
	else if (PyDict_Check(pResult)) {
		std::cout << "Function returned a dictionary:" << std::endl;
		PyObject *key, *value;
		Py_ssize_t pos = 0;
		while (PyDict_Next(pResult, &pos, &key, &value)) {
			std::cout << "Key: ";
			print_pyobject(key);
			std::cout << "Value: ";
			print_pyobject(value);
		}
	}
	else {
		std::cout << "Function returned an unsupported type." << std::endl;
	}
}


inline auto my_c_function(std::vector<double> vec) {

	std::vector<double> result(4);
	// 初始化Python解释器
	Py_Initialize();
	if (!Py_IsInitialized()) {
		std::cerr << "Python initialization failed!" << std::endl;
		return result;
	}

	// 添加Python模块路径
	PyRun_SimpleString("import sys");
	PyRun_SimpleString("sys.path.append('.')");
	// 导入Cython生成的模块
	PyObject* pModule = PyImport_ImportModule("my_adf");
	if (!pModule) {
		std::cerr << "Failed to load module 'my_adf'" << std::endl;
		PyErr_Print();
		Py_Finalize();
		return result;
	}

	// 获取模块中的函数
	PyObject* pFunc = PyObject_GetAttrString(pModule, "adf_test");
	if (!pFunc || !PyCallable_Check(pFunc)) {
		std::cerr << "Cannot find function 'adf_test'" << std::endl;
		Py_XDECREF(pFunc);
		Py_DECREF(pModule);
		Py_Finalize();
		return result;
	}

	// 创建参数列表
	PyObject* pList = PyList_New(0);
	for (int i = 0; i < vec.size(); ++i) {
		PyList_Append(pList, PyFloat_FromDouble(vec[i]));
	}

	// 创建参数元组
	PyObject* pArgs = PyTuple_New(1);
	PyTuple_SetItem(pArgs, 0, pList); // 示例参数

	// 调用函数
	PyObject* pResult = PyObject_CallObject(pFunc, pArgs);

	if (pResult != NULL) {
		Py_ssize_t size = PyTuple_Size(pResult);
		for (Py_ssize_t i = 0; i < size; ++i) {
			PyObject* item = PyTuple_GetItem(pResult, i);
			// print_pyobject(item);
			result[i] = PyFloat_AsDouble(item);
			// std::cout << i << " in tuple  = " << PyFloat_AsDouble(item) << std::endl;
		}
		Py_DECREF(pResult);
	}
	else {
		PyErr_Print();
	}
	// 清理
	Py_XDECREF(pFunc);
	Py_DECREF(pModule);

	// 关闭Python解释器
	Py_Finalize();

	return result;
}
