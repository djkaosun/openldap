/* $OpenLDAP$ */
/*
 * Copyright 1998-1999 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/* ad.c - routines for dealing with attribute descriptions */

#include "portable.h"

#include <stdio.h>

#include <ac/ctype.h>
#include <ac/errno.h>
#include <ac/socket.h>
#include <ac/string.h>
#include <ac/time.h>

#include "ldap_pvt.h"
#include "slap.h"

#ifdef SLAPD_SCHEMA_NOT_COMPAT

int slap_bv2ad(
	struct berval *bv,
	AttributeDescription **ad,
	char **text )
{
	int rtn = LDAP_UNDEFINED_TYPE;
	int i;
	AttributeDescription desc;
	char **tokens;

	assert( *ad != NULL );
	assert( *text != NULL );

	if( bv == NULL || bv->bv_len == 0 ) {
		*text = "empty attribute description";
		return LDAP_UNDEFINED_TYPE;
	}

	/* make sure description is IA5 */
	if( IA5StringValidate( NULL, bv ) != 0 ) {
		*text = "attribute description contains non-IA5 characters";
		return LDAP_UNDEFINED_TYPE;
	}

	tokens = str2charray( bv->bv_val, ";");

	if( tokens == NULL || *tokens == NULL ) {
		*text = "no attribute type";
		goto done;
	}

	desc.ad_type = at_find( *tokens );

	if( desc.ad_type == NULL ) {
		*text = "attribute type undefined";
		goto done;
	}

	desc.ad_flags = SLAP_DESC_NONE;
	desc.ad_lang = NULL;

	for( i=1; tokens[i] != NULL; i++ ) {
		if( strcasecmp( tokens[i], "binary" ) == 0 ) {
			if( desc.ad_flags & SLAP_DESC_BINARY ) {
				*text = "option \"binary\" specified multiple times";
				goto done;
			}

			if(!( desc.ad_type->sat_syntax->ssyn_flags
				& SLAP_SYNTAX_BINARY ))
			{
				/* not stored in binary, disallow option */
				*text = "option \"binary\" with type not supported";
				goto done;
			}

			desc.ad_flags |= SLAP_DESC_BINARY;

		} else if ( strncasecmp( tokens[i], "lang-",
			sizeof("lang-")-1 ) == 0 && tokens[i][sizeof("lang-")-1] )
		{
			if( desc.ad_lang != NULL ) {
				*text = "multiple language tag options specified";
				goto done;
			}
			desc.ad_lang = tokens[i];

			/* normalize to all lower case, it's easy */
			ldap_pvt_str2lower( desc.ad_lang );

		} else {
			*text = "unrecognized option";
			goto done;
		}
	}

	desc.ad_cname = ch_malloc( sizeof( struct berval ) );

	desc.ad_cname->bv_len = strlen( desc.ad_type->sat_cname );
	if( desc.ad_flags & SLAP_DESC_BINARY ) {
		desc.ad_cname->bv_len += sizeof("binary");
	}
	if( desc.ad_lang != NULL ) {
		desc.ad_cname->bv_len += strlen( desc.ad_lang );
	}

	desc.ad_cname = ch_malloc( desc.ad_cname->bv_len + 1 );

	strcpy( desc.ad_cname->bv_val, desc.ad_type->sat_cname );
	strcat( desc.ad_cname->bv_val, ";binary" );
	if( desc.ad_flags & SLAP_DESC_BINARY ) {
		strcat( desc.ad_cname->bv_val, ";binary" );
	}

	if( desc.ad_lang != NULL ) {
		strcat( desc.ad_cname->bv_val, ";" );
		strcat( desc.ad_cname->bv_val, desc.ad_lang );
	}

	*ad = ch_malloc( sizeof( AttributeDescription ) );
	**ad = desc;

	rtn = LDAP_SUCCESS;

done:
	charray_free( tokens );
	return rtn;
}

void
ad_free( AttributeDescription *ad, int freeit )
{
	if( ad == NULL ) return;

	ber_bvfree( ad->ad_cname );
	free( ad->ad_lang );

	if( freeit ) free( ad );
}

#endif

