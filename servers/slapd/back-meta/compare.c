/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1999-2005 The OpenLDAP Foundation.
 * Portions Copyright 2001-2003 Pierangelo Masarati.
 * Portions Copyright 1999-2003 Howard Chu.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by the Howard Chu for inclusion
 * in OpenLDAP Software and subsequently enhanced by Pierangelo
 * Masarati.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/string.h>
#include <ac/socket.h>

#include "slap.h"
#include "../back-ldap/back-ldap.h"
#include "back-meta.h"

int
meta_back_compare( Operation *op, SlapReply *rs )
{
	struct metainfo		*li = ( struct metainfo * )op->o_bd->be_private;
	struct metaconn		*lc;
	struct metasingleconn	*lsc;
	char			*match = NULL,
				*err = NULL;
	struct berval		mmatch = BER_BVNULL;
	int			ncandidates = 0,
				last = 0,
				i,
				count = 0,
				rc,
       				cres = LDAP_SUCCESS,
				rres = LDAP_SUCCESS,
				*msgid;
	dncookie		dc;

	char			*candidates = meta_back_candidates_get( op );

	lc = meta_back_getconn( op, rs, NULL, LDAP_BACK_SENDERR );
	if ( !lc || !meta_back_dobind( lc, op, LDAP_BACK_SENDERR ) ) {
		return rs->sr_err;
	}
	
	msgid = ch_calloc( sizeof( int ), li->mi_ntargets );
	if ( msgid == NULL ) {
		return -1;
	}

	/*
	 * start an asynchronous compare for each candidate target
	 */
	dc.conn = op->o_conn;
	dc.rs = rs;
	dc.ctx = "compareDN";

	for ( i = 0, lsc = &lc->mc_conns[ 0 ]; !META_LAST( lsc ); ++i, ++lsc ) {
		struct berval mdn = BER_BVNULL;
		struct berval mapped_attr = op->orc_ava->aa_desc->ad_cname;
		struct berval mapped_value = op->orc_ava->aa_value;

		if ( candidates[ i ] != META_CANDIDATE ) {
			msgid[ i ] = -1;
			continue;
		}

		/*
		 * Rewrite the compare dn, if needed
		 */
		dc.rwmap = &li->mi_targets[ i ]->mt_rwmap;

		switch ( ldap_back_dn_massage( &dc, &op->o_req_dn, &mdn ) ) {
		case LDAP_UNWILLING_TO_PERFORM:
			rc = 1;
			goto finish;

		default:
			break;
		}

		/*
		 * if attr is objectClass, try to remap the value
		 */
		if ( op->orc_ava->aa_desc == slap_schema.si_ad_objectClass ) {
			ldap_back_map( &li->mi_targets[ i ]->mt_rwmap.rwm_oc,
					&op->orc_ava->aa_value,
					&mapped_value, BACKLDAP_MAP );

			if ( BER_BVISNULL( &mapped_value ) || mapped_value.bv_val[0] == '\0' ) {
				continue;
			}
		/*
		 * else try to remap the attribute
		 */
		} else {
			ldap_back_map( &li->mi_targets[ i ]->mt_rwmap.rwm_at,
				&op->orc_ava->aa_desc->ad_cname,
				&mapped_attr, BACKLDAP_MAP );
			if ( BER_BVISNULL( &mapped_attr ) || mapped_attr.bv_val[0] == '\0' ) {
				continue;
			}

			if ( op->orc_ava->aa_desc->ad_type->sat_syntax == slap_schema.si_syn_distinguishedName )
			{
				dc.ctx = "compareAttrDN";

				switch ( ldap_back_dn_massage( &dc, &op->orc_ava->aa_value, &mapped_value ) )
				{
				case LDAP_UNWILLING_TO_PERFORM:
					rc = 1;
					goto finish;

				default:
					break;
				}
			}
		}
		
		/*
		 * the compare op is spawned across the targets and the first
		 * that returns determines the result; a constraint on unicity
		 * of the result ought to be enforced
		 */
		 rc = ldap_compare_ext( lc->mc_conns[ i ].msc_ld, mdn.bv_val,
				mapped_attr.bv_val, &mapped_value,
				op->o_ctrls, NULL, &msgid[ i ] );

		if ( mdn.bv_val != op->o_req_dn.bv_val ) {
			free( mdn.bv_val );
			BER_BVZERO( &mdn );
		}

		if ( mapped_attr.bv_val != op->orc_ava->aa_desc->ad_cname.bv_val ) {
			free( mapped_attr.bv_val );
			BER_BVZERO( &mapped_attr );
		}

		if ( mapped_value.bv_val != op->orc_ava->aa_value.bv_val ) {
			free( mapped_value.bv_val );
			BER_BVZERO( &mapped_value );
		}

		if ( rc != LDAP_SUCCESS ) {
			/* FIXME: what should we do with the error? */
			continue;
		}

		++ncandidates;
	}

	/*
	 * wait for replies
	 */
	for ( rc = 0, count = 0; ncandidates > 0; ) {

		/*
		 * FIXME: should we check for abandon?
		 */
		for ( i = 0, lsc = &lc->mc_conns[ 0 ]; !META_LAST( lsc ); lsc++, i++ ) {
			int		lrc;
			LDAPMessage	*res = NULL;

			if ( msgid[ i ] == -1 ) {
				continue;
			}

			lrc = ldap_result( lsc->msc_ld, msgid[ i ],
					0, NULL, &res );

			if ( lrc == 0 ) {
				/*
				 * FIXME: should we yield?
				 */
				if ( res ) {
					ldap_msgfree( res );
				}
				continue;

			} else if ( lrc == LDAP_RES_COMPARE ) {
				if ( count > 0 ) {
					rres = LDAP_OTHER;
					rc = -1;
					goto finish;
				}

				rc = ldap_parse_result( lsc->msc_ld, res,
						&rs->sr_err,
						NULL, NULL, NULL, NULL, 1 );
				if ( rc != LDAP_SUCCESS ) {
					rres = rc;
					rc = -1;
					goto finish;
				}
				
				switch ( rs->sr_err ) {
				case LDAP_COMPARE_TRUE:
				case LDAP_COMPARE_FALSE:

					/*
					 * true or flase, got it;
					 * sending to cache ...
					 */
					if ( li->mi_cache.ttl != META_DNCACHE_DISABLED ) {
						( void )meta_dncache_update_entry( &li->mi_cache, &op->o_req_ndn, i );
					}

					count++;
					rc = 0;
					break;

				default:
					rres = slap_map_api2result( rs );

					if ( err != NULL ) {
						free( err );
					}
					ldap_get_option( lsc->msc_ld,
						LDAP_OPT_ERROR_STRING, &err );

					if ( match != NULL ) {
						free( match );
					}
					ldap_get_option( lsc->msc_ld,
						LDAP_OPT_MATCHED_DN, &match );
					
					last = i;
					break;
				}
				msgid[ i ] = -1;
				--ncandidates;

			} else {
				msgid[ i ] = -1;
				--ncandidates;
				if ( res ) {
					ldap_msgfree( res );
				}
				break;
			}
		}
	}

finish:;

	/*
	 * Rewrite the matched portion of the search base, if required
	 * 
	 * FIXME: only the last one gets caught!
	 */
	if ( count == 1 ) {
		if ( match != NULL ) {
			free( match );
			match = NULL;
		}
		
		/*
		 * the result of the compare is assigned to the res code
		 * that will be returned
		 */
		rres = cres;
		
		/*
		 * At least one compare failed with matched portion,
		 * and none was successful
		 */
	} else if ( match != NULL &&  match[0] != '\0' ) {
		struct berval matched;

		ber_str2bv( match, 0, 0, &matched );

		dc.ctx = "matchedDN";
		ldap_back_dn_massage( &dc, &matched, &mmatch );
	}

	if ( rres != LDAP_SUCCESS ) {
		rs->sr_err = rres;
	}
	rs->sr_matched = mmatch.bv_val;
	send_ldap_result( op, rs );
	rs->sr_matched = NULL;

	if ( match != NULL ) {
		if ( mmatch.bv_val != match ) {
			free( mmatch.bv_val );
		}
		free( match );
	}

	if ( msgid ) {
		free( msgid );
	}
	
	return rc;
}

