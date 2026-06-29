#ifndef PHP_DD_H
#define PHP_DD_H

extern zend_module_entry dd_module_entry;
#define phpext_dd_ptr &dd_module_entry

#define PHP_DD_VERSION "0.1.0"

#ifdef ZTS
#include "TSRM.h"
#endif

ZEND_BEGIN_MODULE_GLOBALS(dd)
	zend_bool html_assets_dumped;
ZEND_END_MODULE_GLOBALS(dd)

#if defined(ZTS) && defined(COMPILE_DL_DD)
ZEND_TSRMLS_CACHE_EXTERN()
#endif

#define DD_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(dd, v)

#endif /* PHP_DD_H */
